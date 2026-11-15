# GI v2 probes: noise on a still view and the response to a lamp switched on, at the village stair (1248 1548, feet 106,
# 12:00), 30 FPS (CRYMT_FPSLIMIT: stretches the pace in time). Per variant: a Light entity spawned off near the stair,
# three captures of the GI texture with nothing changing (noise), the lamp on, four captures (response). The GI texture
# per capture: logs\play\key_<time>_*_gi.f16; tools\prof\probe_response.py compares the variants.
#   powershell -File tools\prof\probe_response.ps1 [-Variants probefast,probeslow] [-Tag pr]
param([string[]]$Variants = @('probefast', 'probeslow'), [string]$Tag = 'pr', [int]$Width = 2560, [int]$Height = 1440)
Set-Location D:\Games\Crysis-RE
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not (Test-Path D:\Games\Crysis\Game\zzCryMT.pak)) { 'Game\zzCryMT.pak missing: no GI (run CryMTSettings.exe --gi-install)'; return }
if (Get-Process Crysis, EditorLauncher, Editor -ErrorAction SilentlyContinue) { 'game or editor running - stop'; return }
$env:CRYMT_FPSLIMIT = '30'
$a = '{x=-0.052,y=0,z=-0.559}'
$lamp = '#System.SpawnEntity({class=[[Light]],name=[[crylamp]],position={x=1249.6,y=1550.5,z=107.5},properties={bActive=0,Radius=6,' +
        'Style={fCoronaScale=0,fCoronaDistSizeFactor=1,fCoronaDistIntensityFactor=1,nLightStyle=0},Projector={texture_Texture=[[]],' +
        'bProjectInAllDirs=0,fProjectorFov=90,fProjectorNearPlane=0},Color={clrDiffuse={x=1,y=0.7,z=0.4},fDiffuseMultiplier=4,' +
        'fSpecularMultiplier=1,fHDRDynamic=0},Options={bCastShadow=0,bAffectsThisAreaOnly=0,bUsedInRealTime=1,bFakeLight=0,' +
        'object_RenderGeometry=[[]]},Test={bFillLight=0,bNegativeLight=0}}})'
$on = '#System.GetEntityByName([[crylamp]]):ActivateLight(1)'
$c = "crymt_waitlevel;crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;goto 1248 1548 106;crymt_wait 1;" +
     "#g_gameRules.game:MovePlayer(g_localActor.id,{x=1248,y=1548,z=106},$a);crymt_wait 1;#g_localActor:SetAngles($a);$lamp;" +
     "crymt_wait 8;crymt_capture 1;crymt_wait 1;crymt_capture 1;crymt_wait 0.5;crymt_capture 1;crymt_wait 0.5;crymt_capture 1;" +
     "crymt_wait 0.5;$on;crymt_wait 0.3;crymt_capture 1;crymt_wait 0.4;crymt_capture 1;crymt_wait 1;crymt_capture 1;" +
     "crymt_wait 4;crymt_capture 1;crymt_wait 2;quit"
$out = @()
foreach ($v in $Variants) {
    $before = @(Get-ChildItem D:\Games\Crysis\CryMT\logs\play -File | ForEach-Object { $_.Name })
    & .\tools\prof\spot_run.ps1 -Map village -StartTime 12 -Variants $v -Width $Width -Height $Height -Tag $Tag -Commands $c | Out-Null
    $gi = @(Get-ChildItem D:\Games\Crysis\CryMT\logs\play -File | Where-Object { $before -notcontains $_.Name -and $_.Name -match '_gi\.f16$' } |
            Sort-Object Name | ForEach-Object { $_.FullName })
    $runs = (Select-String -Path D:\Games\Crysis\CryMT\logs\mt\d3d10_census.txt -Pattern 'GI ([0-9]+), voxel updates' | Select-Object -First 1).Matches[0].Groups[1].Value
    "$v : GI passes $runs, GI captures $($gi.Count)"
    $out += "$v=" + ($gi -join ',')
}
Remove-Item Env:CRYMT_FPSLIMIT
$out | Set-Content "D:\Games\Crysis-RE\logs\spot\${Tag}_probe_response.txt"
