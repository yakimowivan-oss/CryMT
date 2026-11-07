# The launcher's preset numbers: every preset on the chosen tests, through the dbghelp loader like a player's start
# (-crymt=<mode>, -crymt-gi=0|1; CryMT.ini is not changed), 2 passes per run, the second counts (the first loads
# textures and compiles shaders). Writes JSON lines and a table.
#   powershell -File tools\prof\preset_bench.ps1 [-Width 2560 -Height 1440] [-Out <file.jsonl>]
param([int]$Width = 2560, [int]$Height = 1440, [string]$Out = "$PSScriptRoot\..\..\logs\preset_bench.jsonl", [int]$MaxSec = 600,
      [string[]]$Tests = @('cpu2', 'gpu', 'village', 'harbor'), [string[]]$Presets = @('vanilla', 'mt', 'dx12', 'dlaa', 'full'))
$g = 'D:\Games\Crysis'
$testDefs = @{
    cpu2 = @{Level = 'ice'; Cfg = 'crymt_q_cpu'; Log = 'benchmark_cpu.log'}
    cpu = @{Level = 'island'; Cfg = 'crymt_q_cpu'; Log = 'benchmark_cpu.log'}
    gpu = @{Level = 'island'; Cfg = 'crymt_q_gpu'; Log = 'benchmark_gpu.log'}
    village = @{Level = 'village'; Cfg = 'crymt_q_autotest'; Log = 'autotest.log'}
    island = @{Level = 'island'; Cfg = 'crymt_q_autotest'; Log = 'autotest.log'}
    harbor = @{Level = 'harbor'; Cfg = 'crymt_q_autotest'; Log = 'autotest.log'}
}
$presetArgs = [ordered]@{vanilla = '-crymt=off'; mt = '-crymt=mt'; dx12 = '-crymt=dx12 -crymt-gi=0'; dlaa = '-crymt=dlaa -crymt-gi=0'; full = '-crymt=dlaa -crymt-gi=1'}
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
foreach ($t in $Tests) {
    $test = $testDefs[$t]
    foreach ($p in $Presets) {
        $log = "$g\Game\Levels\$($test.Level)\$($test.Log)"
        $before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
        $fault = "$g\CryMT\logs\mt\crymt_fault.txt"
        $fb = if (Test-Path $fault) { (Get-Item $fault).Length } else { 0 }
        $t0 = Get-Date
        $gameArgs = "-dx10 -DEVMODE $($presetArgs[$p]) +r_Width $Width +r_Height $Height +r_Fullscreen 0 +map $($test.Level) +exec $($test.Cfg)"
        $proc = Start-Process "$g\Bin64\Crysis.exe" -ArgumentList $gameArgs -WorkingDirectory "$g\Bin64" -PassThru
        $done = $proc.WaitForExit($MaxSec * 1000)
        if (-not $done) { Stop-Process -Id $proc.Id -Force }
        Start-Sleep -Seconds 3
        Get-Process CryMTReport -ErrorAction SilentlyContinue | Stop-Process -Force
        # The log has "Play Time: .., Average FPS: x" and then "Min FPS: y at frame .." for every pass.
        $new = @(Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Skip $before)
        $avgs = @($new | Select-String 'Average FPS:\s*([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
        $mins = @($new | Select-String 'Min FPS:\s*([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
        $passes = @(for ($k = 0; $k -lt [Math]::Min($avgs.Count, $mins.Count); $k++) { @{avg = $avgs[$k]; min = $mins[$k]} })
        $fa = if (Test-Path $fault) { (Get-Item $fault).Length } else { 0 }
        $ok = $done -and $proc.ExitCode -eq 0 -and $passes.Count -ge 2
        $rec = [ordered]@{test = $t; preset = $p; ok = $ok; exit = $(if ($done) { $proc.ExitCode } else { 'timeout' }); fault = ($fa -ne $fb); sec = [int]((Get-Date) - $t0).TotalSeconds
                          avg = $(if ($passes.Count -ge 2) { $passes[1].avg } else { $null }); min = $(if ($passes.Count -ge 2) { $passes[1].min } else { $null })
                          pass1 = $(if ($passes.Count -ge 1) { $passes[0].avg } else { $null }); width = $Width; height = $Height; time = (Get-Date -Format s)}
        ($rec | ConvertTo-Json -Compress) | Add-Content $Out
        "{0,-8} {1,-8} ok={2,-5} avg={3,-7} min={4,-7} pass1={5,-7} {6} s" -f $t, $p, $ok, $rec.avg, $rec.min, $rec.pass1, $rec.sec
    }
}
