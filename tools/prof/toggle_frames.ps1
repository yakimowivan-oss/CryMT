# Flicker A/B within one run (the runs' own levels differ more than the switches do): the player put at CamPos (feet =
# z - 1.2), then for each step its console commands, $Wait s, crymt_capture $Frames (consecutive frames); frames into
# logs\spot\<Tag>_<step>_c<k>.ppm. Steps "name=cmd1,cmd2|name2=...". -Pre runs once the level is up.
#   powershell -File tools\prof\toggle_frames.ps1 -Steps 'on=crymt_contact 2|off=crymt_contact 0|on2=crymt_contact 2'
#     -Pre 'e_wind 0;e_vegetation_bending 0;e_vegetation_wind 0' -Tag tg
param([string]$Map = 'village', [string]$Pos = '1146 592 123', [string]$Angles = '-18 38', [int]$Time = 10, [string]$Tag = 'tg',
      [string[]]$Steps = @('on=crymt_contact 2|off=crymt_contact 0'), [string]$Pre = '', [int]$Frames = 10, [int]$Wait = 3,
      [string]$Variant = 'can0', [int]$Width = 2560, [int]$Height = 1440)
Set-Location D:\Games\Crysis-RE
if (Get-Process Crysis, EditorLauncher, Editor -ErrorAction SilentlyContinue) { 'game or editor running - stop'; return }
$play = 'D:\Games\Crysis\CryMT\logs\play'
$xyz = $Pos -split ' '; $ang = $Angles -split ' '
$p = [math]::Round([double]$ang[0] * [math]::PI / 180, 3); $w = [math]::Round([double]$ang[1] * [math]::PI / 180, 3)
$f = [double]$xyz[2] - 1.2
$a = "{x=$p,y=0,z=$w}"
$pre = if ($Pre) { "$Pre;" } else { '' }
$c = "crymt_waitlevel;${pre}crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;goto $($xyz[0]) $($xyz[1]) $f;crymt_wait 1;" +
     "#g_gameRules.game:MovePlayer(g_localActor.id,{x=$($xyz[0]),y=$($xyz[1]),z=$f},$a);crymt_wait 1;#g_localActor:SetAngles($a);crymt_wait 8;" +
     "crymt_capture 1;crymt_wait 2;" # a warm-up capture
$Steps = @($Steps | ForEach-Object { $_ -split '\|' } | Where-Object { $_ })
$names = @()
foreach ($s in $Steps) {
    $name, $cmd = $s -split '=', 2
    $names += $name
    $c += ($cmd -replace ',', ';') + ";crymt_wait $Wait;crymt_capture $Frames;crymt_wait 3;"
}
$c += 'quit'
$before = @(Get-ChildItem $play -File | ForEach-Object { $_.FullName })
& .\tools\prof\spot_run.ps1 -Map $Map -StartTime $Time -Variants $Variant -Width $Width -Height $Height -Tag $Tag -Commands $c | Out-Null
$new = @(Get-ChildItem $play -File | Where-Object { $before -notcontains $_.FullName })
# each capture's frames share the prefix before "_w1_c<k>.ppm"; captures in time order, the warm-up first
$groups = @($new | Where-Object { $_.Name -match '_w1_c\d+\.ppm$' } | Group-Object { $_.Name -replace '_w1_c\d+\.ppm$', '' } | Sort-Object Name)
for ($i = 1; $i -lt $groups.Count -and $i - 1 -lt $names.Count; $i++) {
    foreach ($x in $groups[$i].Group) {
        $k = [int]($x.Name -replace '.*_c(\d+)\.ppm$', '$1')
        Copy-Item $x.FullName "logs\spot\${Tag}_$($names[$i - 1])_c$k.ppm" -Force
    }
}
$new | Remove-Item -Force -ErrorAction SilentlyContinue
"steps $($names -join ', '): captures $($groups.Count - 1)"
