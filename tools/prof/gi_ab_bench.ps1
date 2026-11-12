# GI variants against each other on the developers' demos, through the dbghelp loader with -crymt=dlaa -crymt-gi=1, the
# variant as environment variables over CryMT.ini (not changed); 2 passes per run, the second counts. Runs interleave
# (test by test, the variants in turn) so drift between runs falls on all variants alike.
#   powershell -File tools\prof\gi_ab_bench.ps1 [-Width 2560 -Height 1440] [-Tests harbor,gpu] [-Rounds 1]
#   [-PreCfg name]: a Game\Config\<name>.cfg executed before the test's (cvar experiments, after the spec is applied)
param([int]$Width = 2560, [int]$Height = 1440, [string]$Out = "$PSScriptRoot\..\..\logs\gi_ab_bench.jsonl", [int]$MaxSec = 600,
      [string[]]$Tests = @('harbor', 'gpu', 'village'), [int]$Rounds = 1, [string]$PreCfg = "", [string[]]$Variants = @('default', 'nogeom', 'screen', 'nogi'))
$g = 'D:\Games\Crysis'
$testDefs = @{
    cpu2 = @{Level = 'ice'; Cfg = 'crymt_q_cpu'; Log = 'benchmark_cpu.log'}
    gpu = @{Level = 'island'; Cfg = 'crymt_q_gpu'; Log = 'benchmark_gpu.log'}
    village = @{Level = 'village'; Cfg = 'crymt_q_autotest'; Log = 'autotest.log'}
    harbor = @{Level = 'harbor'; Cfg = 'crymt_q_autotest'; Log = 'autotest.log'}
    mine = @{Level = 'mine'; Cfg = 'crymt_q_autotest'; Log = 'autotest.log'}
    fleet = @{Level = 'fleet'; Cfg = 'crymt_q_autotest'; Log = 'autotest.log'}
}
$variantDefs = [ordered]@{
    default = @{}
    nogeom = @{CRYMT_GI_GEOMETRY = '0'}
    screen = @{CRYMT_GI_SCREEN = '1'}
    nogi = @{CRYMT_GI = '0'}
    nocache = @{CRYMT_GI_GEOMCACHE = '0'}
}
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$Tests = @($Tests | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($r in 1..$Rounds) {
foreach ($t in $Tests) {
    $test = $testDefs[$t]
    foreach ($v in $Variants) {
        foreach ($name in 'CRYMT_GI_GEOMETRY', 'CRYMT_GI_SCREEN', 'CRYMT_GI', 'CRYMT_GI_GEOMCACHE') { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        foreach ($kv in $variantDefs[$v].GetEnumerator()) { Set-Item "Env:$($kv.Key)" $kv.Value }
        $log = "$g\Game\Levels\$($test.Level)\$($test.Log)"
        $before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
        $t0 = Get-Date
        $gi = if ($v -eq 'nogi') { '0' } else { '1' }
        $pre = if ($PreCfg) { "+exec $PreCfg " } else { '' }
        $gameArgs = "-dx10 -DEVMODE -crymt=dlaa -crymt-gi=$gi +r_Width $Width +r_Height $Height +r_Fullscreen 0 +map $($test.Level) $pre+exec $($test.Cfg)"
        $proc = Start-Process "$g\Bin64\Crysis.exe" -ArgumentList $gameArgs -WorkingDirectory "$g\Bin64" -PassThru
        $done = $proc.WaitForExit($MaxSec * 1000)
        if (-not $done) { Stop-Process -Id $proc.Id -Force }
        Start-Sleep -Seconds 3
        Get-Process CryMTReport -ErrorAction SilentlyContinue | Stop-Process -Force
        $new = @(Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Skip $before)
        $avgs = @($new | Select-String 'Average FPS:\s*([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
        $mins = @($new | Select-String 'Min FPS:\s*([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
        $ok = $done -and $avgs.Count -ge 2 -and $mins.Count -ge 2
        $rec = [ordered]@{test = $t; variant = $v; round = $r; ok = $ok; sec = [int]((Get-Date) - $t0).TotalSeconds
                          avg = $(if ($avgs.Count -ge 2) { $avgs[1] } else { $null }); min = $(if ($mins.Count -ge 2) { $mins[1] } else { $null })
                          pass1 = $(if ($avgs.Count -ge 1) { $avgs[0] } else { $null }); width = $Width; height = $Height; time = (Get-Date -Format s)}
        ($rec | ConvertTo-Json -Compress) | Add-Content $Out
        "{0,-8} {1,-8} r{2} ok={3,-5} avg={4,-7} min={5,-7} pass1={6,-7} {7} s" -f $t, $v, $r, $ok, $rec.avg, $rec.min, $rec.pass1, $rec.sec
    }
}
}
foreach ($name in 'CRYMT_GI_GEOMETRY', 'CRYMT_GI_SCREEN', 'CRYMT_GI', 'CRYMT_GI_GEOMCACHE') { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
