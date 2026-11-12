param([string[]]$Variants = 'full,nogi,geomcache,nogeom,nozown', [string]$Pre = '', [int]$Width = 5120, [int]$Height = 1440, [int]$Every = 20,
      [string]$GotoPos = '1103 1390 137', [string]$Pos = '{x=1103,y=1390,z=137}', [string]$Angles = '{x=-0.0873,y=0,z=-1.117}',
      [string]$Tag = 'spot', [string]$Capture = '', [string]$Commands = '', [switch]$Sample, [int]$SampleSec = 30)
# Frame times at one place on village (default: the user's spot of 2026-11-12, CamPos 1103 1390 139, Angl -5 0 -64 at
# 7:00, flying: the ground there is under water at ~98). CryMT's test commands, $Every s apart after the loader's start
# commands: fly mode (DevMode F3, action "flymode"), goto, the move with the view angles, the entity's angles (turns the
# view; actor:SetAngles does not), crymt_statsreset (the census counts from here: GPU busy, per-pass GPU times at this
# place), a Lua line logging the place, quit (CryMT writes its logs first). spot_report.py summarises the window from the
# reset to quit. -Sample: CrySampler2 (200 Hz, $SampleSec s) from the reset on, into logs\spot\<Tag>_<variant>.*
# (symprof.py / rtfuncs.py <prefix> <thread>). $Capture: CRYMT_CAPTURE_SEC for a full-size PPM (its clock runs ~20 s
# ahead of the commands').
#   powershell -File tools\prof\spot_run.ps1 -Variants nogi,nogi_gpu [-Sample]
$g = 'D:\Games\Crysis'
$out = "D:\Games\Crysis-RE\logs\spot"
New-Item -ItemType Directory -Force $out | Out-Null
$defs = [ordered]@{
    full = @{Gi = '1'; Env = @{}}
    nogi = @{Gi = '0'; Env = @{}}
    geomcache = @{Gi = '1'; Env = @{CRYMT_GI_GEOMCACHE = '1'}}
    nogeom = @{Gi = '1'; Env = @{CRYMT_GI_GEOMETRY = '0'}}
    nozown = @{Gi = '1'; Env = @{CRYMT_ZOWN = '0'}}
    full_gpu = @{Gi = '1'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'}}
    nogi_gpu = @{Gi = '0'; Env = @{CRYMT_MARKERS = '1'; CRYMT_GPUPASSES = '1'}}
}
$focus = "$g\CryMT\logs\mt\crymt_focus.txt"
foreach ($v in ($Variants | ForEach-Object { $_ -split ',' })) {
    $d = $defs[$v]
    if (-not $d) { "unknown variant $v"; continue }
    $vars = @{CRYMT_FRAMELOG = '1'; CRYMT_GI_STARTTIME = '7'; CRYMT_TESTCMDSEC = "$Every"
              CRYMT_TESTCMD = "#g_localActor.actor:SimulateOnAction('flymode',1,1);goto $GotoPos;" +
                              "#g_gameRules.game:MovePlayer(g_localActor.id,$Pos,$Angles);#g_localActor:SetAngles($Angles);crymt_statsreset;" +
                              "#local p=g_localActor:GetWorldPos() local a=g_localActor:GetAngles() " +
                              "System.LogAlways('crymt spot '..math.floor(p.x)..' '..math.floor(p.y)..' '..math.floor(p.z)..' '..a.x..' '..a.z);" +
                              "#System.LogAlways('crymt spot end');quit"}
    if ($Commands) { $vars.CRYMT_TESTCMD = $Commands }
    if ($Pre) { $vars.CRYMT_TESTCMD = "$Pre;" + $vars.CRYMT_TESTCMD } # e.g. -Pre 'cl_hud 0' ($Every s earlier, all later)
    if ($Capture) { $vars.CRYMT_CAPTURE_SEC = $Capture; $vars.CRYMT_CAPTURE_COUNT = '1'; $vars.CRYMT_CAPTURE_TAG = "${Tag}_$v" }
    foreach ($e in $d.Env.GetEnumerator()) { $vars[$e.Key] = $e.Value }
    foreach ($e in $vars.GetEnumerator()) { Set-Item "Env:$($e.Key)" $e.Value }
    $focusLines = if (Test-Path $focus) { (Get-Content $focus).Count } else { 0 }
    $gameArgs = "-dx10 -DEVMODE -crymt=dlaa -crymt-gi=$($d.Gi) +r_Width $Width +r_Height $Height +r_Fullscreen 0 +map village +exec girecord"
    $p = Start-Process "$g\Bin64\Crysis.exe" -ArgumentList $gameArgs -WorkingDirectory "$g\Bin64" -PassThru
    if ($Sample) {
        $t0 = Get-Date
        while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $Every * 8 + 60) {
            Start-Sleep -Seconds 1
            if (Get-Content $focus -ErrorAction SilentlyContinue | Select-Object -Skip $focusLines | Select-String -SimpleMatch 'census counts reset' -Quiet) { break }
        }
        if (-not $p.HasExited) { & 'D:\Games\Crysis-RE\tools\prof\CrySampler2.exe' $p.Id $SampleSec 200 "$out\${Tag}_$v" 5 }
    }
    if (-not $p.WaitForExit(($Every * 9 + 90) * 1000)) { Stop-Process -Id $p.Id -Force; "$v force-stopped" }
    Start-Sleep -Seconds 3
    foreach ($f in 'crymt_frames.txt', 'crymt_detail.txt', 'd3d10_census.txt', 'crymt_focus.txt', 'crymt_stats.txt') {
        Copy-Item "$g\CryMT\logs\mt\$f" "$out\${Tag}_${v}_$f" -Force -ErrorAction SilentlyContinue
    }
    if ($Sample) { Copy-Item "$g\CryMT\logs\mt\crymt_frames.txt" "$out\${Tag}_$v.frames.txt" -Force -ErrorAction SilentlyContinue }
    Copy-Item "$g\Game.log" "$out\${Tag}_${v}_Game.log" -Force -ErrorAction SilentlyContinue
    foreach ($e in $vars.Keys) { Remove-Item "Env:$e" -ErrorAction SilentlyContinue }
    "$v done"
}
