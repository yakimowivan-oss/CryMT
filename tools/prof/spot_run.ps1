param([string]$Variants = 'full,nogi,geomcache,nogeom,nozown', [int]$Width = 5120, [int]$Height = 1440, [int]$Every = 20,
      [string]$GotoPos = '1103 1390 137', [string]$Pos = '{x=1103,y=1390,z=137}', [string]$Angles = '{x=-0.0873,y=0,z=-1.117}', [string]$Tag = 'spot', [string]$Capture = '', [string]$Commands = '')
# The user's place on village (their capture: CamPos 1103 1390 139, Angl -5 0 -64 at 7:00, flying: the ground there is
# under water at ~98). CryMT's test commands, $Every s apart after the loader's start commands: fly mode (DevMode F3,
# action "flymode") and the move with the view angles in one Lua line (goto drops the player, actor:SetAngles leaves the
# view), then r_DisplayInfo 1, echo, quit. Measured between the r_DisplayInfo and quit commands (frame numbers in
# crymt_focus.txt) with CryMT's frame log.
# $Capture: CRYMT_CAPTURE_SEC for a full-size PPM (logs\play\<Tag>_<variant>_w1_c1.ppm).
$g = 'D:\Games\Crysis'
$out = "D:\Games\Crysis-RE\logs\spot"
New-Item -ItemType Directory -Force $out | Out-Null
$defs = [ordered]@{
    full = @{Gi = '1'; Env = @{}}
    nogi = @{Gi = '0'; Env = @{}}
    geomcache = @{Gi = '1'; Env = @{CRYMT_GI_GEOMCACHE = '1'}}
    nogeom = @{Gi = '1'; Env = @{CRYMT_GI_GEOMETRY = '0'}}
    nozown = @{Gi = '1'; Env = @{CRYMT_ZOWN = '0'}}
}
foreach ($v in ($Variants -split ',')) {
    $d = $defs[$v]
    $vars = @{CRYMT_FRAMELOG = '1'; CRYMT_GI_STARTTIME = '7'; CRYMT_TESTCMDSEC = "$Every"
              CRYMT_TESTCMD = "#g_localActor.actor:SimulateOnAction('flymode',1,1);goto $GotoPos;" +
                              "#g_gameRules.game:MovePlayer(g_localActor.id,$Pos,$Angles);#g_localActor:SetAngles($Angles);" +
                              "r_DisplayInfo 1;#local p=g_localActor:GetWorldPos() local a=g_localActor:GetAngles() " +
                              "System.LogAlways('crymt spot '..math.floor(p.x)..' '..math.floor(p.y)..' '..math.floor(p.z)..' '..a.x..' '..a.z);quit"}
    if ($Commands) { $vars.CRYMT_TESTCMD = $Commands }
    if ($Capture) { $vars.CRYMT_CAPTURE_SEC = $Capture; $vars.CRYMT_CAPTURE_COUNT = '1'; $vars.CRYMT_CAPTURE_TAG = "${Tag}_$v" }
    foreach ($e in $d.Env.GetEnumerator()) { $vars[$e.Key] = $e.Value }
    foreach ($e in $vars.GetEnumerator()) { Set-Item "Env:$($e.Key)" $e.Value }
    $gameArgs = "-dx10 -DEVMODE -crymt=dlaa -crymt-gi=$($d.Gi) +r_Width $Width +r_Height $Height +r_Fullscreen 0 +map village +exec girecord"
    $p = Start-Process "$g\Bin64\Crysis.exe" -ArgumentList $gameArgs -WorkingDirectory "$g\Bin64" -PassThru
    if (-not $p.WaitForExit(($Every * 7 + 90) * 1000)) { Stop-Process -Id $p.Id -Force; "$v force-stopped" }
    Start-Sleep -Seconds 3
    foreach ($f in 'crymt_frames.txt', 'crymt_detail.txt', 'd3d10_census.txt', 'crymt_focus.txt') {
        Copy-Item "$g\CryMT\logs\mt\$f" "$out\${Tag}_${v}_$f" -Force -ErrorAction SilentlyContinue
    }
    Copy-Item "$g\Game.log" "$out\${Tag}_${v}_Game.log" -Force -ErrorAction SilentlyContinue
    foreach ($e in $vars.Keys) { Remove-Item "Env:$e" -ErrorAction SilentlyContinue }
    "$v done"
}
