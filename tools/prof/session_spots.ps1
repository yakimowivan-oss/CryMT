# GI variants at several spots in ONE game run (the user, 2026-11-15: a restart per variant took 10-15 min of waiting):
# the level loads once; per spot the camera is put there, then per variant its crymt_gictl commands, a wait, a capture.
# Captures go to logs\spot\out_<spot>_<variant>_frame.ppm / _gi.f16 / _dist.r32 as user_spots.ps1 writes them, so
# bands.py and gi_bands.py read them as they are.
#   powershell -File tools\prof\session_spots.ps1 -Spots y1,y3,y5 -Variants 'nogi=gi 0','cur=gi 1;farprobes 0','far=gi 1;farprobes 1'
#   [-Map mine] [-Time 12] [-Settle 4] [-Wait 3] [-Mode dlaa]
param([string[]]$Spots = @('y1', 'y3', 'y5'), [string[]]$Variants = @('nogi=gi 0', 'gi=gi 1'), [string]$Map = 'mine', [int]$Time = 12,
      [double]$Settle = 4, [double]$Wait = 3, [int]$Width = 2560, [int]$Height = 1440, [string]$Mode = 'dlaa')
Set-Location D:\Games\Crysis-RE
$Spots = @($Spots | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (Get-Process Crysis, EditorLauncher, Editor -ErrorAction SilentlyContinue) { 'game or editor running - stop'; return }
if (-not (Test-Path D:\Games\Crysis\Game\zzCryMT.pak)) { 'Game\zzCryMT.pak missing: no GI (run CryMTSettings.exe --gi-install)'; return }
# map, CamPos x y z, Angl pitch yaw (degrees) as the overlay shows them; feet = z - 1.2 (as user_spots.ps1)
$all = @{
    y1 = @(2389, 2345, 236, -1, -1); y2 = @(2388, 2319, 236, -2, -2); y3 = @(2387, 2306, 236, -2, -2)
    y4 = @(2387, 2293, 236, -2, -2); y5 = @(2386, 2283, 236, -2, 2)
    m1 = @(2401, 2274, 236, 0, -159); stair = @(1248, 1548, 107.2, -3, -32)
}
$vars = @()
foreach ($v in $Variants) { $p = $v -split '=', 2; $vars += , @($p[0], $p[1]) }
$order = @()
$c = "crymt_waitlevel;crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;crymt_capture 1;crymt_wait 1" # warm-up capture (no dumps)
foreach ($name in $Spots) {
    $s = $all[$name]
    if (-not $s) { "unknown spot $name"; return }
    $pt = [math]::Round($s[3] * [math]::PI / 180, 3); $yw = [math]::Round($s[4] * [math]::PI / 180, 3); $f = $s[2] - 1.2
    $a = "{x=$pt,y=0,z=$yw}"
    $c += ";goto $($s[0]) $($s[1]) $f;crymt_wait 1;#g_gameRules.game:MovePlayer(g_localActor.id,{x=$($s[0]),y=$($s[1]),z=$f},$a);" +
          "crymt_wait 1;#g_localActor:SetAngles($a);crymt_wait $Settle"
    foreach ($v in $vars) {
        foreach ($cmd in ($v[1] -split ';')) { $c += ";crymt_gictl $cmd" }
        $c += ";crymt_wait $Wait;crymt_capture 1;crymt_wait 1.5"
        $order += , @($name, $v[0])
    }
}
$c += ";crymt_wait 2;quit"
$play = 'D:\Games\Crysis\CryMT\logs\play'
$before = @(Get-ChildItem $play -File | ForEach-Object { $_.Name })
& .\tools\prof\spot_run.ps1 -Map $Map -StartTime $Time -Variants 'farprobes' -Width $Width -Height $Height -Tag session -Commands $c -Mode $Mode | Out-Null
$new = @(Get-ChildItem $play -File | Where-Object { $before -notcontains $_.Name })
$stamps = @($new | Where-Object { $_.Name -match '_w1_c1\.ppm$' } | ForEach-Object { $_.Name.Substring(0, 16) } | Sort-Object -Unique)
if ($stamps.Count -ne $order.Count + 1) { "captures $($stamps.Count), expected $($order.Count + 1): check logs\play"; }
for ($i = 0; $i -lt [math]::Min($order.Count, $stamps.Count - 1); $i++) {
    $st = $stamps[$i + 1]; $o = "D:\Games\Crysis-RE\logs\spot\out_$($order[$i][0])_$($order[$i][1])"
    foreach ($pair in @(@('_w1_c1.ppm', '_frame.ppm'), @('_gi.f16', '_gi.f16'), @('_dist.r32', '_dist.r32'))) {
        $src = $new | Where-Object { $_.Name.StartsWith($st) -and $_.Name.EndsWith($pair[0]) } | Select-Object -First 1
        if ($src) { Copy-Item $src.FullName "$o$($pair[1])" -Force }
    }
}
"session: $($order.Count) captures for $($Spots.Count) spots x $($vars.Count) variants"
