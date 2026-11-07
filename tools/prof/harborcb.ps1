# Harbor CPU test (benchmark_harbor + autotest demo, 2 runs) in the user's DLAA config, GI off, 3840x1080 window, with
# and without extra cvars per variant: "default" or "cvar=value[,cvar=value]". Prints FPS per demo run, GPU busy, main wait.
param([string[]]$Variants = @('e_cbuffer=0', 'default'))
$base = @{CRYMT_FILL='1'; CRYMT_TERRAINOWN='1'; CRYMT_D3D12='1'; CRYMT_D3DSTREAM='1'; CRYMT_DLSS='1'; CRYMT_BORDERLESS='1'; CRYMT_ZOWN='1'; CRYMT_ZPACKETS='1'; CRYMT_ZGROUPS='all'; CRYMT_ZGROUPRELAX='1'; CRYMT_TEXRELAX='1'}
foreach ($v in $Variants) {
    $extra = @('+r_Fullscreen','0','+r_Width','3840','+r_Height','1080','+r_DynTexMaxSize','256','+r_UseEdgeAA','0','+pp_RSFDebugWrite','0')
    if ($v -ne 'default') { foreach ($kv in $v -split ',') { $p = $kv -split '='; $extra += @("+$($p[0])", $p[1]) } }
    foreach ($k in $base.Keys) { Set-Item "env:$k" $base[$k] }
    & "D:\Games\Crysis-RE\tools\prof\run_profile.ps1" -Tag 'harbor_cb' -Seconds 2 -Hz 50 -Level harbor -Script benchmark_harbor -DemoLog autotest -MaxRunSec 420 -Extra $extra 2>$null | Out-Null
    foreach ($k in $base.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    $runs = (Select-String -Path D:\Games\Crysis-RE\logs\prof2\harbor_cb.bench.log -Pattern 'Average FPS: [0-9.]+|Min FPS: [0-9.]+' | ForEach-Object { $_.Matches.Value -replace '.*: ', '' }) -join ' '
    $busy = (Select-String -Path D:\Games\Crysis-RE\logs\mt\d3d10_census.txt -Pattern 'GPU busy [0-9.]+' | Select-Object -Last 1).Matches.Value
    $wait = (Select-String -Path D:\Games\Crysis-RE\logs\mt\crymt_stats.txt -Pattern 'mainWaitMs/frame=[0-9.]+' | Select-Object -First 1).Matches.Value
    "$v (crash=" + (Select-String -Path D:\Games\Crysis\Game.log -SimpleMatch 'CRASH DETECTED' -Quiet) + "): avg/min FPS per run: $runs; $busy; $wait"
}
