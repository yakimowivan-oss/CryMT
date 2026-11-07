# Far-vegetation flicker on the gitest demo (island, the mountain at the end): consecutive frames from world frame 2115,
# the user's config, then one feature off each; per variant the frames whose mountain area jumps away from both
# neighbours (flickaba.py, A-B-A). Variants: full, nogi, notexrelax, nozown, nodlss, noasync, terrainfence, noterrainown, nofill, native (CRYMT_MODE=0:
# the flush on main inside RenderInner as in the original, with the D3D12 back end and captures).
param([string[]]$Variants = @('full'), [string]$Frames = '2115', [int]$Count = 40, [string[]]$Extra = @(), [int]$Stream = 0)
$base = @{CRYMT_FILL='1'; CRYMT_TERRAINOWN='1'; CRYMT_D3D12='1'; CRYMT_D3DSTREAM='1'; CRYMT_DLSS='1'; CRYMT_BORDERLESS='1'; CRYMT_ZOWN='1'; CRYMT_ZPACKETS='1'; CRYMT_ZGROUPS='all'; CRYMT_ZGROUPRELAX='1'; CRYMT_TEXRELAX='1'; CRYMT_GI='1'; CRYMT_GI_RADIUS='2.5'; CRYMT_GI_BOUNCE='10'; CRYMT_GI_AO='4'; CRYMT_GI_TEMPORAL='0.7'; CRYMT_GI_VOXELSTRENGTH='3'; CRYMT_GI_VOXELAO='1.2'; CRYMT_CAPTURE_FRAME=$Frames; CRYMT_CAPTURE_COUNT="$Count"}
foreach ($v in $Variants) {
    $vars = $base.Clone()
    $tag = "vd_" + ($v -replace '[^A-Za-z0-9]', '_')
    $vars['CRYMT_CAPTURE_TAG'] = $tag
    if ($Stream) { $vars.Remove('CRYMT_CAPTURE_FRAME'); $vars['CRYMT_CAPTURE_STREAM'] = "$Stream"; Remove-Item "D:\Games\Crysis-RE\logs\play\${tag}_stream.raw" -ErrorAction SilentlyContinue }
    switch ($v) {
        'nozown' { foreach ($k in 'CRYMT_ZOWN','CRYMT_ZPACKETS','CRYMT_ZGROUPS','CRYMT_ZGROUPRELAX') { $vars.Remove($k) } }
        'notexrelax' { $vars.Remove('CRYMT_TEXRELAX') }
        'noasync' { $vars['CRYMT_ASYNCPSO'] = '0' }
        'nogi' { $vars.Remove('CRYMT_GI') }
        'nodlss' { $vars.Remove('CRYMT_DLSS') }
        'terrainfence' { $vars['CRYMT_TERRAINFENCE'] = '1' }
        { $_ -like 'mask*' } { $vars['CRYMT_FENCE'] = $v.Substring(4) } # CRYMT_FENCE bits (C3DEngine::Update parts)
        { $_ -like 'fence*' } { $vars['CRYMT_FENCEAT'] = $v.Substring(5) } # fence<site>:<target>, see CryMT.cpp CRYMT_FENCEAT
        'noterrainown' { $vars.Remove('CRYMT_TERRAINOWN') }
        'nofill' { $vars.Remove('CRYMT_FILL'); $vars.Remove('CRYMT_TERRAINOWN') }
        'native' { foreach ($k in 'CRYMT_FILL','CRYMT_TERRAINOWN','CRYMT_ZOWN','CRYMT_ZPACKETS','CRYMT_ZGROUPS','CRYMT_ZGROUPRELAX') { $vars.Remove($k) } }
    }
    Remove-Item "D:\Games\Crysis-RE\logs\play\${tag}_*.ppm" -ErrorAction SilentlyContinue
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    $cvars = @('+r_Fullscreen','0','+r_Width','1920','+r_Height','1080','+r_DynTexMaxSize','256','+r_UseEdgeAA','0','+pp_RSFDebugWrite','0','+r_MotionBlur','0') + $Extra
    & "D:\Games\Crysis-RE\tools\prof\run_profile.ps1" -Tag "island_$tag" -Seconds 2 -Hz 50 -Level island -Script giplay -DemoLog gitest -MaxRunSec 200 -Extra $cvars -Native:($v -eq 'native') 2>$null | Out-Null
    foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    $files = (Get-ChildItem "D:\Games\Crysis-RE\logs\play\${tag}_w1_*.ppm" -ErrorAction SilentlyContinue | Sort-Object { [int]($_.BaseName -replace '.*_c', '') }).FullName
    "$v (crash=" + (Select-String -Path D:\Games\Crysis\Game.log -SimpleMatch 'CRASH DETECTED' -Quiet) + ", frames $($files.Count))"
    if ($Stream) { python D:\Games\Crysis-RE\tools\prof\flickstream.py "D:\Games\Crysis-RE\logs\play\${tag}_stream.raw" }
    if ($files.Count -ge 3) { python D:\Games\Crysis-RE\tools\prof\flickaba.py "D:\Games\Crysis-RE\logs\play\$tag.png" @files threshold=10 region=0,80,960,420 | Select-Object -First 1 }
}
