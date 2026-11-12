param([string[]]$Variants = 'full', [int]$Seconds = 240, [string]$Tag = 'repro')
# GPU hang reproduction (2026-11-12): village from its start, the player standing, $Seconds s, then quit. Per run:
# whether the driver reset the GPU (System log nvlddmkm 153) and d3d12.txt's "device removed" with the DRED lines.
#   powershell -File tools\prof\hang_repro.ps1 -Variants 'full_safe,full' [-Seconds 240]
$out = 'D:\Games\Crysis-RE\logs\spot'
foreach ($v in ($Variants | ForEach-Object { $_ -split ',' })) {
    $t0 = Get-Date
    & D:\Games\Crysis-RE\tools\prof\spot_run.ps1 -Variants $v -Tag $Tag -Commands "crymt_waitlevel;crymt_wait $Seconds;quit" | Out-Null
    Get-Process Crysis -ErrorAction SilentlyContinue | Stop-Process -Force
    for ($i = 0; $i -lt 20 -and (Get-Process Crysis -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
    $resets = @(Get-WinEvent -FilterHashtable @{LogName = 'System'; ProviderName = 'nvlddmkm'; StartTime = $t0} -ErrorAction SilentlyContinue)
    Copy-Item 'D:\Games\Crysis\CryMT\logs\mt\d3d12.txt' "$out\${Tag}_${v}_d3d12.txt" -Force -ErrorAction SilentlyContinue
    $removed = Select-String -Path "$out\${Tag}_${v}_d3d12.txt" -Pattern 'device removed' -ErrorAction SilentlyContinue | Select-Object -First 1
    "{0} {1}: {2} s, GPU resets {3}, {4}" -f (Get-Date -Format 'HH:mm'), $v, [int]((Get-Date) - $t0).TotalSeconds, $resets.Count, ($(if ($removed) { $removed.Line } else { 'no device removal' }))
}
