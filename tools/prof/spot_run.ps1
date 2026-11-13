param([string[]]$Variants = 'full,nogi,geomcache,nogeom,nozown', [string]$Pre = '', [int]$Width = 5120, [int]$Height = 1440,
      [int]$Settle = 6, [int]$Measure = 20,
      [string]$GotoPos = '1103 1390 137', [string]$Pos = '{x=1103,y=1390,z=137}', [string]$Angles = '{x=-0.0873,y=0,z=-1.117}',
      [string]$Tag = 'spot', [string]$Capture = '', [string]$Commands = '', [switch]$Sample, [int]$SampleSec = 15,
      [string]$Mode = 'dlaa', # -Mode mt: the "MT only" preset (the stock D3D10 renderer), dx12, dlaa
      [string]$Exec = 'girecord', # -Exec giplay_rooms: the user's girooms demo (VisArea rooms, lamps; quits at its end)
      [string]$Map = 'village', # -Map mine: the level loaded
      [string]$StartTime = '7') # hour of the day (mine: 12, its sun is ~13 degrees at 7)
# Frame times at one place on village (default: the user's spot of 2026-11-12, CamPos 1103 1390 139, Angl -5 0 -64 at
# 7:00, flying: the ground there is under water at ~98), about 50 s a run. CryMT's test commands (crymt_wait/
# crymt_waitlevel pace them): once the level runs, the player's physics off (holds it in the air; SimulateOnAction
# ('flymode') did not turn fly mode on - the user kept it up with F3 in the runs before 2026-11-12 14:00), goto, the move with the view
# angles, the entity's angles (turns the view; actor:SetAngles does not), $Settle s, crymt_statsreset (the census counts
# from here: GPU busy, per-pass GPU times at this place), $Measure s, a Lua line logging the place, quit (CryMT writes its
# logs first). spot_report.py / spot_phases.py summarise the window from the reset to quit. -Pre: commands once the level
# runs (e.g. 'cl_hud 0'). -Sample: CrySampler2 (200 Hz, $SampleSec s) from the reset on, into logs\spot\<Tag>_<variant>.*
# (symprof.py / rtfuncs.py / stackof.py <prefix> <thread>). $Capture: CRYMT_CAPTURE_SEC for a full-size PPM.
#   powershell -File tools\prof\spot_run.ps1 -Variants nogi,nogi_gpu [-Sample]
$g = 'D:\Games\Crysis'
$out = "D:\Games\Crysis-RE\logs\spot"
New-Item -ItemType Directory -Force $out | Out-Null
$defs = [ordered]@{
    full = @{Gi = '1'; Env = @{}}
    nogi = @{Gi = '0'; Env = @{}}
    nogi_nobf = @{Gi = '0'; Env = @{CRYMT_BFIDENTITY = '0'}}
    nogi_nosf = @{Gi = '0'; Env = @{CRYMT_SFQUEUE = '0'}}
    nogi_nodred = @{Gi = '0'; Env = @{CRYMT_DRED = '0'}}
    full_safe = @{Gi = '1'; Env = @{CRYMT_BFIDENTITY = '0'; CRYMT_FILLLIGHTQ = '0'; CRYMT_Q17 = '0'}}
    full_dbg = @{Gi = '1'; Env = @{CRYMT_D3D12DEBUG = '1'}}
    full_nobf = @{Gi = '1'; Env = @{CRYMT_BFIDENTITY = '0'}}
    full_nofl = @{Gi = '1'; Env = @{CRYMT_FILLLIGHTQ = '0'}}
    full_noq17 = @{Gi = '1'; Env = @{CRYMT_Q17 = '0'}}
    nogi_nophys = @{Gi = '0'; Env = @{CRYMT_PHYSTURN = '0'}}
    full_nophys = @{Gi = '1'; Env = @{CRYMT_PHYSTURN = '0'}}
    full_nosf = @{Gi = '1'; Env = @{CRYMT_SFQUEUE = '0'}}
    geomcache = @{Gi = '1'; Env = @{CRYMT_GI_GEOMCACHE = '1'}}
    nogeom = @{Gi = '1'; Env = @{CRYMT_GI_GEOMETRY = '0'}}
    nozown = @{Gi = '1'; Env = @{CRYMT_ZOWN = '0'}}
    # Screen-space shadows A/B (sss_ab.ps1 gives the commands: crymt_contact 2/1/0 with crymt_capture each): GPU per pass, 9:00.
    sss = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_GI_STARTTIME = '9'}}
    sss_r200 = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_GI_STARTTIME = '9'; CRYMT_SSS_REACH = '200'}}
    sss_r1000 = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_GI_STARTTIME = '9'; CRYMT_SSS_REACH = '1000'}}
    sss_r4000 = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_GI_STARTTIME = '9'; CRYMT_SSS_REACH = '4000'}}
    sss_16 = @{Gi = '1'; Env = @{CRYMT_GI_STARTTIME = '16'}} # the sun facing the overview spot's view
    full_gpu = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'}}
    # GI without the engine cube's face draws in the voxel geometry (before 2026-11-13 evening), GPU per pass.
    nocubegeom_gpu = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_GI_CUBEGEOM = '0'}}
    # GI without the cube faces' surfaces injected into the voxels (CRYMT_GI_CUBEINJECT, before 2026-11-13 night).
    noinject_gpu = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_GI_CUBEINJECT = '0'}}
    full_early = @{Gi = '1'; Env = @{CRYMT_EARLYBARRIER = '1'; CRYMT_THREADLEVEL = '1'}} # main released before shadow prep
    nogi_early = @{Gi = '0'; Env = @{CRYMT_EARLYBARRIER = '1'; CRYMT_THREADLEVEL = '1'}}
    full_nojitter = @{Gi = '1'; Env = @{CRYMT_DLSS_JITTER = '0'}} # DLAA without the camera jitter (water update test)
    # Motion vector check (crymt_capture N in -Commands dumps N frames: colour, vectors, motion blur target, foliage
    # mask; tools\prof\mv_check.py): no jitter, so a still frame pair differs only by motion and light.
    mvseq = @{Gi = '1'; Env = @{CRYMT_MVSEQ = '1'; CRYMT_DLSS_JITTER = '0'}}
    # DLAA smear check (tools\prof\dlaa_seq.py): the same sequence with the jitter on as played, and with its sign flipped.
    mvseqj = @{Gi = '1'; Env = @{CRYMT_MVSEQ = '1'}}
    mvseqj_flip = @{Gi = '1'; Env = @{CRYMT_MVSEQ = '1'; CRYMT_DLSS_JITTER = '--'}}
    # The engine cube under main's frame id as before 2026-11-13 07:10 (crash A/B).
    cube_oldframe = @{Gi = '1'; Env = @{CRYMT_ENVCUBE_OWNFRAME = '0'}}
    # The cube's own frame id (default 1 from 07:05 to ~21:00 on 2026-11-13: it lost the lamps in VisAreas).
    cube_ownframe = @{Gi = '1'; Env = @{CRYMT_ENVCUBE_OWNFRAME = '1'}}
    # No engine cube at all: water planar, materials the game's maps (a CRYMT_ENVMAP line in CryMT.ini overrides this).
    nocube = @{Gi = '1'; Env = @{CRYMT_WATERCUBE = '0'; CRYMT_ENVMAP = 'game'}}
    # The voxel light pass's march towards the sun: the whole grid (128 cells) instead of 24, GPU per pass.
    sunmarch128_gpu = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_GI_SUNMARCH = '128'}}
    # Water reflections (give r_WaterReflections 0 in the commands for 1 and 2): planar, the voxel cube, SSR + the cube.
    water_planar = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'}}
    water_cube = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_WATERCUBE = '1'}}
    water_ssr = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'; CRYMT_WATERCUBE = '2'}}
    nogi_gpu = @{Gi = '0'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'}}
}
$focus = "$g\CryMT\logs\mt\crymt_focus.txt"
$pre = if ($Pre) { "$Pre;" } else { '' }
foreach ($v in ($Variants | ForEach-Object { $_ -split ',' })) {
    $d = $defs[$v]
    if (-not $d) { "unknown variant $v"; continue }
    $vars = @{CRYMT_FRAMELOG = '1'; CRYMT_GI_STARTTIME = $StartTime; CRYMT_TESTCMDSEC = '0'
              CRYMT_TESTCMD = "crymt_waitlevel;${pre}crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;" +
                              "goto $GotoPos;crymt_wait 1;#g_gameRules.game:MovePlayer(g_localActor.id,$Pos,$Angles);crymt_wait 1;" +
                              "#g_localActor:SetAngles($Angles);crymt_wait $Settle;crymt_statsreset;crymt_wait $Measure;" +
                              "#local p=g_localActor:GetWorldPos() local a=g_localActor:GetAngles() " +
                              "System.LogAlways('crymt spot '..math.floor(p.x)..' '..math.floor(p.y)..' '..math.floor(p.z)..' '..a.x..' '..a.z);quit"}
    if ($Commands) { $vars.CRYMT_TESTCMD = $Commands }
    if ($Capture) { $vars.CRYMT_CAPTURE_SEC = $Capture; $vars.CRYMT_CAPTURE_COUNT = '1'; $vars.CRYMT_CAPTURE_TAG = "${Tag}_$v" }
    foreach ($e in $d.Env.GetEnumerator()) { $vars[$e.Key] = $e.Value }
    foreach ($e in $vars.GetEnumerator()) { Set-Item "Env:$($e.Key)" $e.Value }
    $focusLines = if (Test-Path $focus) { (Get-Content $focus).Count } else { 0 }
    $gameArgs = "-dx10 -DEVMODE -crymt=$Mode -crymt-gi=$($d.Gi) +r_Width $Width +r_Height $Height +r_Fullscreen 0 +map $Map +exec $Exec"
    foreach ($f in 'crymt_vis.txt', 'crymt_lightlog.txt') { Remove-Item "$g\CryMT\logs\mt\$f" -ErrorAction SilentlyContinue } # no stale copies
    $p = Start-Process "$g\Bin64\Crysis.exe" -ArgumentList $gameArgs -WorkingDirectory "$g\Bin64" -PassThru
    if ($Sample) {
        $t0 = Get-Date
        while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 180) {
            Start-Sleep -Milliseconds 500
            if (Get-Content $focus -ErrorAction SilentlyContinue | Select-Object -Skip $focusLines | Select-String -SimpleMatch 'census counts reset' -Quiet) { break }
        }
        if (-not $p.HasExited) { & 'D:\Games\Crysis-RE\tools\prof\CrySampler2.exe' $p.Id $SampleSec 200 "$out\${Tag}_$v" 5 }
    }
    if (-not $p.WaitForExit((180 + $Settle + $Measure) * 1000)) { Stop-Process -Id $p.Id -Force; "$v force-stopped" }
    Start-Sleep -Seconds 2
    foreach ($f in 'crymt_frames.txt', 'crymt_detail.txt', 'd3d10_census.txt', 'crymt_focus.txt', 'crymt_stats.txt', 'crymt_vis.txt', 'crymt_lightlog.txt') {
        Copy-Item "$g\CryMT\logs\mt\$f" "$out\${Tag}_${v}_$f" -Force -ErrorAction SilentlyContinue
    }
    if ($Sample) { Copy-Item "$g\CryMT\logs\mt\crymt_frames.txt" "$out\${Tag}_$v.frames.txt" -Force -ErrorAction SilentlyContinue }
    Copy-Item "$g\Game.log" "$out\${Tag}_${v}_Game.log" -Force -ErrorAction SilentlyContinue
    foreach ($e in $vars.Keys) { Remove-Item "Env:$e" -ErrorAction SilentlyContinue }
    "$v done"
}
