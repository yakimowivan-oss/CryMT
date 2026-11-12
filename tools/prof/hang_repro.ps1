param([string[]]$Variants = 'full', [int]$Seconds = 240, [string]$Tag = 'repro', [switch]$Spot)
# GPU hang reproduction (2026-11-12): village from its start, the player standing, $Seconds s, then quit. -Spot: at the
# user's overview spot instead (spot_run's default place: 1103 1390 137, physics off, the same view). Per run:
# whether the driver reset the GPU (System log nvlddmkm 153) and d3d12.txt's "device removed" with the DRED lines.
#   powershell -File tools\prof\hang_repro.ps1 -Variants 'full_safe,full' [-Seconds 240] [-Spot]
$out = 'D:\Games\Crysis-RE\logs\spot'
$move = ''
if ($Spot) {
    $move = 'crymt_wait 2;#g_localActor:EnablePhysics(false);crymt_wait 1;goto 1103 1390 137;crymt_wait 1;' +
            '#g_gameRules.game:MovePlayer(g_localActor.id,{x=1103,y=1390,z=137},{x=-0.0873,y=0,z=-1.117});crymt_wait 1;' +
            '#g_localActor:SetAngles({x=-0.0873,y=0,z=-1.117});'
}
foreach ($v in ($Variants | ForEach-Object { $_ -split ',' })) {
    $t0 = Get-Date
    & D:\Games\Crysis-RE\tools\prof\spot_run.ps1 -Variants $v -Tag $Tag -Commands "crymt_waitlevel;${move}crymt_wait $Seconds;quit" | Out-Null
    Get-Process Crysis -ErrorAction SilentlyContinue | Stop-Process -Force
    for ($i = 0; $i -lt 20 -and (Get-Process Crysis -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
    $resets = @(Get-WinEvent -FilterHashtable @{LogName = 'System'; ProviderName = 'nvlddmkm'; StartTime = $t0} -ErrorAction SilentlyContinue)
    Copy-Item 'D:\Games\Crysis\CryMT\logs\mt\d3d12.txt' "$out\${Tag}_${v}_d3d12.txt" -Force -ErrorAction SilentlyContinue
    $removed = Select-String -Path "$out\${Tag}_${v}_d3d12.txt" -Pattern 'device removed' -ErrorAction SilentlyContinue | Select-Object -First 1
    "{0} {1}: {2} s, GPU resets {3}, {4}" -f (Get-Date -Format 'HH:mm'), $v, [int]((Get-Date) - $t0).TotalSeconds, $resets.Count, ($(if ($removed) { $removed.Line } else { 'no device removal' }))
}
