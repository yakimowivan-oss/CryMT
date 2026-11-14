# The user's spots (screenshots of 2026-11-14), captured per GI variant: frame, bounce light and raw occlusion into
# logs\spot\out_<spot>_<variant>_*.ppm.
#   powershell -File tools\prof\user_spots.ps1 [-Variants dirl1_gpu,probes_s1,probes_hyb] [-Time 10] [-Spots u1,u4,u5,u6]
param([string[]]$Variants = @('dirl1_gpu', 'probes_s1', 'probes_hyb'), [int]$Time = 10, [string[]]$Spots = @('u1', 'u4', 'u5', 'u6'))
Set-Location D:\Games\Crysis-RE
# powershell -File passes "u5,u6" as one string
$Spots = @($Spots | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (Get-Process Crysis, EditorLauncher, Editor -ErrorAction SilentlyContinue) { 'game or editor running - stop'; return }
$play = 'D:\Games\Crysis\CryMT\logs\play'
# map, CamPos x y z, Angl pitch yaw (degrees) as the overlay shows them; feet = z - 1.2
$all = @{
    # first set: a room with a lamp, a classroom, a first floor's ceiling, a shed
    u1 = @('village', 1119, 666, 116, -7, 40); u4 = @('village', 1242, 1557, 106, 0, 27)
    u5 = @('village', 1249, 1553, 112, 1, 179); u6 = @('village', 1325, 1539, 106, 4, -47)
    # second set (marked problems with probes): the lamp room's ceiling and floor, a classroom ceiling, a room ceiling,
    # the stair's upper ceiling; Sponza: dark blotches at a wall's foot, a light crescent on an arch
    v1 = @('village', 1118, 666, 116, 1, 38); v2 = @('village', 1240, 1547, 106, 10, 142)
    v3 = @('village', 1244, 1558, 106, 4, -4); v4 = @('village', 1250, 1548, 111, 13, 43)
    s1 = @('multiplayer/ia/sponza', 116, 121, 15, -3, 114); s2 = @('multiplayer/ia/sponza', 141, 116, 15, 17, 126)
}
foreach ($name in $Spots) {
    $s = $all[$name]
    if (-not $s) { "unknown spot $name"; continue }
    $p = [math]::Round($s[4] * [math]::PI / 180, 3); $w = [math]::Round($s[5] * [math]::PI / 180, 3); $f = $s[3] - 1.2
    $a = "{x=$p,y=0,z=$w}"
    $c = "crymt_waitlevel;crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;goto $($s[1]) $($s[2]) $f;crymt_wait 1;" +
         "#g_gameRules.game:MovePlayer(g_localActor.id,{x=$($s[1]),y=$($s[2]),z=$f},$a);crymt_wait 1;#g_localActor:SetAngles($a);" +
         "crymt_wait 10;crymt_capture 3;crymt_wait 3;quit"
    $t = if ($s[0] -like '*sponza*') { 12 } else { $Time }
    foreach ($v in $Variants) {
        $before = @(Get-ChildItem $play -File | ForEach-Object { $_.FullName })
        & .\tools\prof\spot_run.ps1 -Map $s[0] -StartTime $t -Variants $v -Width 2560 -Height 1440 -Tag "out_$name" -Commands $c | Out-Null
        $new = Get-ChildItem $play -File | Where-Object { $before -notcontains $_.FullName }
        foreach ($pat in @(@('*_w1_c3.ppm', 'frame'), @('*_gi_rgb.ppm', 'girgb'), @('*_raw_occ.ppm', 'rawocc'))) {
            $k = $new | Where-Object { $_.Name -like $pat[0] } | Select-Object -First 1
            if ($k) { Copy-Item $k.FullName "D:\Games\Crysis-RE\logs\spot\out_${name}_${v}_$($pat[1]).ppm" -Force }
        }
        $new | Remove-Item -Force -ErrorAction SilentlyContinue
        "$name $v done"
    }
}
