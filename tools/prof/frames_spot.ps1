# A run of consecutive frames at one place per variant (flicker frame to frame, A/B of builds or switches): the player
# put at CamPos (feet = z - 1.2) with the view angles as the overlay shows them, $Wait s, crymt_capture $Frames, quit.
# Frames into logs\spot\<Tag>_<variant>_c<k>.ppm, the stats as spot_run copies them (<Tag>_<variant>_crymt_stats.txt).
#   powershell -File tools\prof\frames_spot.ps1 -Variants lcc_on,lcc_off -Pos '1241 1551 106' -Angles '0 48' -Tag lamp
param([string[]]$Variants = @('lcc_on', 'lcc_off'), [string]$Map = 'village', [string]$Pos = '1241 1551 106',
      [string]$Angles = '0 48', [int]$Time = 10, [int]$Frames = 10, [int]$Wait = 10, [string]$Tag = 'frames',
      [int]$Width = 2560, [int]$Height = 1440, [string]$Pre = '', [string]$Mode = 'dlaa') # -Pre 'e_gsm_cache 0': console commands once the level runs; -Mode mt|dx12|dlaa
Set-Location D:\Games\Crysis-RE
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (Get-Process Crysis, EditorLauncher, Editor -ErrorAction SilentlyContinue) { 'game or editor running - stop'; return }
$play = 'D:\Games\Crysis\CryMT\logs\play'
$xyz = $Pos -split ' '; $ang = $Angles -split ' '
$p = [math]::Round([double]$ang[0] * [math]::PI / 180, 3); $w = [math]::Round([double]$ang[1] * [math]::PI / 180, 3)
$f = [double]$xyz[2] - 1.2
$a = "{x=$p,y=0,z=$w}"
$pre = if ($Pre) { "$Pre;" } else { '' }
$c = "crymt_waitlevel;${pre}crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;goto $($xyz[0]) $($xyz[1]) $f;crymt_wait 1;" +
     "#g_gameRules.game:MovePlayer(g_localActor.id,{x=$($xyz[0]),y=$($xyz[1]),z=$f},$a);crymt_wait 1;#g_localActor:SetAngles($a);" +
     "crymt_wait $Wait;crymt_statsreset;crymt_capture $Frames;crymt_wait 3;quit"
foreach ($v in $Variants) {
    $before = @(Get-ChildItem $play -File | ForEach-Object { $_.FullName })
    & .\tools\prof\spot_run.ps1 -Map $Map -StartTime $Time -Variants $v -Width $Width -Height $Height -Tag $Tag -Commands $c -Mode $Mode | Out-Null
    $new = Get-ChildItem $play -File | Where-Object { $before -notcontains $_.FullName }
    $k = 0
    foreach ($n in ($new | Where-Object { $_.Name -match '_w1_c\d+\.ppm$' } | Sort-Object { [int]($_.Name -replace '.*_c(\d+)\.ppm$', '$1') })) {
        $k++
        Copy-Item $n.FullName "D:\Games\Crysis-RE\logs\spot\${Tag}_${v}_c$k.ppm" -Force
    }
    $new | Remove-Item -Force -ErrorAction SilentlyContinue
    "$v done: $k frames"
}
