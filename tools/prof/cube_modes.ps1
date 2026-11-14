# The engine cube's faces at one place under several panel settings, in one run: for each step the console commands,
# a wait (the cube redraws its faces), crymt_capture 1 (frame + <tag>_envcube0..5.ppm) and crymt_cubepic (one picture).
# Files into logs\spot\cube_<Tag>_<step>_envcube<face>.ppm / _frame.ppm / _pic.bmp.
#   powershell -File tools\prof\cube_modes.ps1 -Map island -Pos '1593 2764 104' -Angles '5 -15' -Tag isl
#     -Steps 'none=crymt_envcubecull 1','inv=crymt_envcubecull 2','face=crymt_envcubecull 3','eng=crymt_envcubecull 0'
param([string]$Map = 'island', [string]$Pos = '1593 2764 104', [string]$Angles = '5 -15', [int]$Time = 10, [string]$Tag = 'cube',
      [string[]]$Steps = @('none=crymt_envcubecull 1', 'inv=crymt_envcubecull 2', 'face=crymt_envcubecull 3', 'eng=crymt_envcubecull 0'),
      [string]$Variant = 'cube_down', [int]$Width = 2560, [int]$Height = 1440)
Set-Location D:\Games\Crysis-RE
if (Get-Process Crysis, EditorLauncher, Editor -ErrorAction SilentlyContinue) { 'game or editor running - stop'; return }
$play = 'D:\Games\Crysis\CryMT\logs\play'
$xyz = $Pos -split ' '; $ang = $Angles -split ' '
$p = [math]::Round([double]$ang[0] * [math]::PI / 180, 3); $w = [math]::Round([double]$ang[1] * [math]::PI / 180, 3)
$f = [double]$xyz[2] - 1.2
$a = "{x=$p,y=0,z=$w}"
$c = "crymt_waitlevel;crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;goto $($xyz[0]) $($xyz[1]) $f;crymt_wait 1;" +
     "#g_gameRules.game:MovePlayer(g_localActor.id,{x=$($xyz[0]),y=$($xyz[1]),z=$f},$a);crymt_wait 1;#g_localActor:SetAngles($a);crymt_wait 8;" +
     "crymt_capture 1;crymt_wait 2;" # the first capture of a run has no extra dumps (no cube faces): a warm-up
$names = @()
# powershell -File passes the list as one string: steps split at '|' (commands inside a step at ',')
$Steps = @($Steps | ForEach-Object { $_ -split '\|' } | Where-Object { $_ })
foreach ($s in $Steps) {
    $name, $cmd = $s -split '=', 2
    $names += $name
    $c += ($cmd -replace ',', ';') + ";crymt_wait 3;crymt_capture 1;crymt_wait 2;crymt_cubepic;crymt_wait 2;"
}
$c += 'quit'
$before = @(Get-ChildItem $play -File | ForEach-Object { $_.FullName })
& .\tools\prof\spot_run.ps1 -Map $Map -StartTime $Time -Variants $Variant -Width $Width -Height $Height -Tag "cube_$Tag" -Commands $c | Out-Null
$new = @(Get-ChildItem $play -File | Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime)
# captures in order: each step's frame + six faces, then its picture
$frames = @($new | Where-Object { $_.Name -match '_w1_c1\.ppm$' } | Select-Object -Skip 1) # after the warm-up
for ($i = 0; $i -lt $names.Count; $i++) {
    $n = $names[$i]
    if ($i -lt $frames.Count) {
        $prefix = $frames[$i].FullName -replace '_w1_c1\.ppm$', ''
        Copy-Item $frames[$i].FullName "logs\spot\cube_${Tag}_${n}_frame.ppm" -Force
        for ($k = 0; $k -lt 6; $k++) {
            if (Test-Path "${prefix}_envcube$k.ppm") { Copy-Item "${prefix}_envcube$k.ppm" "logs\spot\cube_${Tag}_${n}_envcube$k.ppm" -Force }
        }
        if (Test-Path "${prefix}_envcube.bmp") { Copy-Item "${prefix}_envcube.bmp" "logs\spot\cube_${Tag}_${n}_pic.bmp" -Force }
    }
}
$pics = @($new | Where-Object { $_.Name -like '*_envcube.bmp' })
"new files: " + (($new | ForEach-Object Name) -join ', ')
$new | Remove-Item -Force -ErrorAction SilentlyContinue
"steps $($names -join ', '): frames $($frames.Count), pictures $($pics.Count)"
