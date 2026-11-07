# Vegetation pop-in / flicker on a still island view, per variant: 16 consecutive frames ~40 s in, quit at ~46 s.
# Variants: full (the user's config), then one feature off each: nozown, notexrelax, noasync, nogi, nodlss.
param([string[]]$Variants = @('full'), [int]$Count = 4)
$base = @{CRYMT_FILL='1'; CRYMT_TERRAINOWN='1'; CRYMT_D3D12='1'; CRYMT_D3DSTREAM='1'; CRYMT_DLSS='1'; CRYMT_BORDERLESS='1'; CRYMT_ZOWN='1'; CRYMT_ZPACKETS='1'; CRYMT_ZGROUPS='all'; CRYMT_ZGROUPRELAX='1'; CRYMT_TEXRELAX='1'; CRYMT_GI='1'; CRYMT_GI_BOUNCE='10'; CRYMT_GI_AO='4'; CRYMT_GI_VOXELSTRENGTH='3'; CRYMT_GI_VOXELAO='1.2'; CRYMT_GI_KEYS='1'; CRYMT_GI_STARTTIME='12'; CRYMT_CAPTURE_SEC='34,36,38,40,42'; CRYMT_CAPTURE_COUNT="$Count"; CRYMT_TESTCMD='echo a;quit'; CRYMT_TESTCMDSEC='47'}
foreach ($v in $Variants) {
    $vars = $base.Clone()
    $vars['CRYMT_CAPTURE_TAG'] = "vp_$v"
    switch ($v) {
        'nozown' { foreach ($k in 'CRYMT_ZOWN','CRYMT_ZPACKETS','CRYMT_ZGROUPS','CRYMT_ZGROUPRELAX') { $vars.Remove($k) } }
        'notexrelax' { $vars.Remove('CRYMT_TEXRELAX') }
        'noasync' { $vars['CRYMT_ASYNCPSO'] = '0' }
        'nogi' { $vars.Remove('CRYMT_GI') }
        'nodlss' { $vars.Remove('CRYMT_DLSS') }
        'mt' { foreach ($k in 'CRYMT_D3D12','CRYMT_D3DSTREAM','CRYMT_DLSS','CRYMT_GI') { $vars.Remove($k) } }
    }
    Remove-Item "D:\Games\Crysis-RE\logs\play\vp_${v}_*.ppm" -ErrorAction SilentlyContinue
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    & "D:\Games\Crysis-RE\tools\prof\run_profile.ps1" -Tag "island_vp_$v" -Seconds 1 -Hz 50 -Level island -Script girecord -DemoLog none -MaxRunSec 90 -Extra @('+r_Fullscreen','0','+r_Width','1920','+r_Height','1080','+r_DynTexMaxSize','256','+r_UseEdgeAA','0','+pp_RSFDebugWrite','0','+e_vegetation_bending','0','+e_vegetation_wind','0','+e_wind','0','+e_particles','0') 2>$null | Out-Null
    foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    $files = (Get-ChildItem "D:\Games\Crysis-RE\logs\play\vp_${v}_*.ppm" -ErrorAction SilentlyContinue | Sort-Object { [int]($_.BaseName -replace '.*_w(\d+)_c.*', '$1') * 100 + [int]($_.BaseName -replace '.*_c', '') }).FullName
    "$v (crash=" + (Select-String -Path D:\Games\Crysis\Game.log -SimpleMatch 'CRASH DETECTED' -Quiet) + ", frames $($files.Count))"
    if ($files.Count -ge 2) { python D:\Games\Crysis-RE\tools\prof\popframes.py "D:\Games\Crysis-RE\logs\play\vp_$v.png" @files }
}
