param([string]$Dll, [int]$Runs = 3, [int]$Seconds = 230, [string]$Tag = 'olddll')
# GPU hang A/B with another CryMT.dll (an older build): installs it, runs village from its start $Runs times for $Seconds s
# each (the start commands the old loader gave through CRYMT_TESTCMD, one a frame), counts the driver's GPU resets
# (System log nvlddmkm), then puts the current DLL back.
$g = 'D:\Games\Crysis'
$cur = "$g\CryMT\CryMT.dll"
$backup = "$g\CryMT\CryMT.dll.current"
Copy-Item $cur $backup -Force
Copy-Item $Dll $cur -Force
try {
    for ($r = 1; $r -le $Runs; $r++) {
        $t0 = Get-Date
        $env:CRYMT_TESTCMD = 'r_DynTexMaxSize 256;e_gsm_cache 1;r_UseEdgeAA 0;pp_RSFDebugWrite 0;pp_RSFDebugWriteOnLoad 0;e_cbuffer 0'
        $env:CRYMT_TESTCMDSEC = '0'
        $env:CRYMT_GI_STARTTIME = '7'
        $p = Start-Process "$g\Bin64\Crysis.exe" -ArgumentList '-dx10 -DEVMODE -crymt=dlaa -crymt-gi=1 +r_Width 5120 +r_Height 1440 +r_Fullscreen 0 +map village +exec girecord' -WorkingDirectory "$g\Bin64" -PassThru
        Remove-Item Env:CRYMT_TESTCMD, Env:CRYMT_TESTCMDSEC, Env:CRYMT_GI_STARTTIME
        $p.WaitForExit($Seconds * 1000) | Out-Null
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        for ($i = 0; $i -lt 20 -and (Get-Process Crysis -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
        $resets = @(Get-WinEvent -FilterHashtable @{LogName = 'System'; ProviderName = 'nvlddmkm'; StartTime = $t0} -ErrorAction SilentlyContinue)
        $removed = Select-String -Path "$g\CryMT\logs\mt\d3d12.txt" -Pattern 'device removed' -ErrorAction SilentlyContinue | Select-Object -First 1
        "{0} {1} run {2}: GPU resets {3}, {4}" -f (Get-Date -Format 'HH:mm'), $Tag, $r, $resets.Count, ($(if ($removed) { $removed.Line } else { 'no device removal' }))
    }
}
finally {
    Copy-Item $backup $cur -Force
    'current DLL restored'
}
