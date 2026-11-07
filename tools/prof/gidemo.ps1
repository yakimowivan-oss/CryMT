# GI variants on the user's gitest demo (island 11:30, DLAA), motion blur off: frames captured at fixed demo frames.
param([string[]]$Variants = @('off', 'v13', 'up'), [string]$Frames = '1150,1900', [int]$Count = 4, [int]$Width = 1920, [int]$Height = 1080, [int]$Fullscreen = 0, [string]$Ao = '4', [string]$VoxelAo = '1.2', [string]$Radius = '2.5', [string[]]$More = @(), [string]$Script = 'giplay', [int]$MaxRunSec = 200, [string]$Level = 'island', [string]$Demo = 'gitest', [hashtable]$Override = @{}, [switch]$Native)
$base = @{CRYMT_FILL='1'; CRYMT_TERRAINOWN='1'; CRYMT_D3D12='1'; CRYMT_D3DSTREAM='1'; CRYMT_DLSS='1'; CRYMT_BORDERLESS='1'; CRYMT_ZOWN='1'; CRYMT_ZPACKETS='1'; CRYMT_ZGROUPS='all'; CRYMT_ZGROUPRELAX='1'; CRYMT_TEXRELAX='1'; CRYMT_CAPTURE_FRAME=$Frames; CRYMT_CAPTURE_COUNT="$Count"}
$gi = @{CRYMT_GI='1'; CRYMT_GI_RADIUS=$Radius; CRYMT_GI_BOUNCE='6.7'; CRYMT_GI_AO=$Ao; CRYMT_GI_TEMPORAL='0.7'; CRYMT_GI_VOXELSTRENGTH='3'; CRYMT_GI_VOXELAO=$VoxelAo} # the user's ini
$all = @($base.Keys) + @($gi.Keys) + @('CRYMT_GI_UPSAMPLE', 'CRYMT_GI_NEAREST', 'CRYMT_GI_VOXELVIEW', 'CRYMT_GI_VOXELS', 'CRYMT_GI_VOXELSTRENGTH', 'CRYMT_GI_VOXELAO', 'CRYMT_CAPTURE_TAG')
foreach ($v in $Variants) {
    $tag = "gd_$v"
    Remove-Item "D:\Games\Crysis-RE\logs\play\${tag}_*.ppm" -ErrorAction SilentlyContinue
    foreach ($k in $base.Keys) { Set-Item "env:$k" $base[$k] }
    foreach ($k in $Override.Keys) { Set-Item "env:$k" $Override[$k] }
    $env:CRYMT_CAPTURE_TAG = $tag
    if ($v -ne 'off') { foreach ($k in $gi.Keys) { Set-Item "env:$k" $gi[$k] } }
    if ($v -eq 'v13') { $env:CRYMT_GI_UPSAMPLE = '0' }
    if ($v -in @('up', 'v13', 'upnear', 'upfixed')) { $env:CRYMT_GI_VOXELS = '0' } # screen part only (the cones come with GI by default)
    if ($v -eq 'upnear') { $env:CRYMT_GI_NEAREST = '1' }
    if ($v -like 'vox*') { $env:CRYMT_GI_VOXELVIEW = $v.Substring(3) }
    if ($v -match '^c(\d)$') { $env:CRYMT_GI_VOXELS = $Matches[1]; $env:CRYMT_GI_VOXELVIEW = '1' }
    if ($v -match '^g(\d)$') { $env:CRYMT_GI_VOXELS = $Matches[1] }
    & "D:\Games\Crysis-RE\tools\prof\run_profile.ps1" -Tag "island_$tag" -Seconds 2 -Hz 50 -Level $Level -Script $Script -DemoLog $Demo -MaxRunSec $MaxRunSec -Native:$Native -Extra (@('+r_Fullscreen',"$Fullscreen",'+r_Width',"$Width",'+r_Height',"$Height",'+r_DynTexMaxSize','256','+r_UseEdgeAA','0','+pp_RSFDebugWrite','0','+r_MotionBlur','0') + $More) 2>$null | Out-Null
    foreach ($k in $all) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $Override.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    $crash = Select-String -Path D:\Games\Crysis\Game.log -SimpleMatch 'CRASH DETECTED' -Quiet
    $fps = (Select-String -Path "D:\Games\Crysis-RE\logs\prof2\island_$tag.bench.log" -Pattern "Average FPS" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
    "$v (crash=$crash): " + (Get-ChildItem "D:\Games\Crysis-RE\logs\play\${tag}_*.ppm" -ErrorAction SilentlyContinue).Count + " frames; $fps"
    Select-String -Path D:\Games\Crysis-RE\logs\mt\d3d10_census.txt -Pattern "GPU busy" -ErrorAction SilentlyContinue | Select-Object -Last 1 | ForEach-Object { "    " + $_.Line.Trim() }
}
