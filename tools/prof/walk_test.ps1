# Walk test (the user, 2026-11-14: walking, a surface's GI shading changes with the distance: probes near, cones mid,
# none far): the camera backs off a target along one line of sight and takes a frame at each distance; walk_test.py then
# reads the target's brightness at the screen's centre per distance, against the run without GI.
#   powershell -File tools\prof\walk_test.ps1 [-Variants nogi,dirl1_gpu,probes_hyb] [-Target "2404.74 2264.25 236"]
#     [-Yaw -159] [-Map mine] [-Time 12] [-Tag walk_m1] [-Distances "6,10,15,22,32,45,65"]
param([string[]]$Variants = @('nogi', 'dirl1_gpu', 'probes_hyb'), [string]$Target = '2404.74 2264.25 236', [double]$Yaw = -159,
      [double]$Pitch = 0, [string]$Map = 'mine', [int]$Time = 12, [string]$Tag = 'walk_m1', [string]$Distances = '6,10,15,22,32,45,65')
Set-Location D:\Games\Crysis-RE
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (Get-Process Crysis, EditorLauncher, Editor -ErrorAction SilentlyContinue) { 'game or editor running - stop'; return }
$play = 'D:\Games\Crysis\CryMT\logs\play'
$out = "D:\Games\Crysis-RE\logs\walk"
New-Item -ItemType Directory -Force $out | Out-Null
$t = $Target -split ' ' | ForEach-Object { [double]$_ }
$q = $Yaw * [math]::PI / 180; $p = $Pitch * [math]::PI / 180
$fwd = @((-[math]::Sin($q) * [math]::Cos($p)), ([math]::Cos($q) * [math]::Cos($p)), [math]::Sin($p))
$ang = "{x=$([math]::Round($p, 3)),y=0,z=$([math]::Round($q, 3))}"
$dists = $Distances -split ',' | ForEach-Object { [double]$_ }
$c = "crymt_waitlevel;crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1"
foreach ($d in $dists) {
    $x = [math]::Round($t[0] - $fwd[0] * $d, 2); $y = [math]::Round($t[1] - $fwd[1] * $d, 2); $z = [math]::Round($t[2] - $fwd[2] * $d - 1.2, 2)
    $c += ";goto $x $y $z;crymt_wait 1;#g_gameRules.game:MovePlayer(g_localActor.id,{x=$x,y=$y,z=$z},$ang);crymt_wait 1;#g_localActor:SetAngles($ang);crymt_wait 5;crymt_capture 1;crymt_wait 2"
}
$c += ";quit"
foreach ($v in $Variants) {
    $before = @(Get-ChildItem $play -File | ForEach-Object { $_.FullName })
    & .\tools\prof\spot_run.ps1 -Map $Map -StartTime $Time -Variants $v -Width 2560 -Height 1440 -Tag $Tag -Commands $c | Out-Null
    $new = Get-ChildItem $play -File | Where-Object { $before -notcontains $_.FullName }
    $frames = @($new | Where-Object { $_.Name -match '_c1\.ppm$' } | Sort-Object LastWriteTime)
    for ($i = 0; $i -lt $frames.Count -and $i -lt $dists.Count; $i++) {
        Copy-Item $frames[$i].FullName "$out\${Tag}_${v}_$($dists[$i]).ppm" -Force
    }
    $new | Remove-Item -Force -ErrorAction SilentlyContinue
    "$v : $($frames.Count) frames"
}
