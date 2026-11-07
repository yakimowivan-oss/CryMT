param(
    [string]$Tag = "prof",
    [int]$Seconds = 50,
    [int]$Hz = 200,
    [int]$Threads = 5,
    [switch]$Native,
    [string[]]$Extra = @(),
    [string]$Level = "island",
    [string]$Script = "benchmark_cpu",
    [string]$DemoLog = "benchmark_cpu",
    [int]$MaxRunSec = 200
)
# Benchmark with timestamped multi-thread sampling; mode 2 render thread unless -Native.
$root = "D:\Games\Crysis"; $out = "D:\Games\Crysis-RE\logs\prof2"; New-Item -ItemType Directory -Force $out | Out-Null
$build = "D:\Games\Crysis-RE\src\crymt\build"
$gameArgs = @('-dx10', '-DEVMODE', '+r_Fullscreen', '0', '+r_Width', '1280', '+r_Height', '720') + $Extra + @('+map', $Level, '+exec', $Script)
Remove-Item "D:\Games\Crysis-RE\logs\mt\crymt_frames.txt", "$root\Game\Levels\$Level\$DemoLog.log" -ErrorAction SilentlyContinue
if ($Native) {
    $env:CRYMT_MODE = "0"
} else {
    $env:CRYMT_MODE = "2"; $env:CRYMT_LEVEL = "2"
}
$env:CRYMT_FRAMELOG = "1"
$id = & "$build\Inject.exe" "$build\CryMT.dll" $root @gameArgs
$env:CRYMT_MODE = $null; $env:CRYMT_LEVEL = $null; $env:CRYMT_FRAMELOG = $null
$p = Get-Process -Id ([int]$id)
$t0 = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 120) {
    Start-Sleep -Seconds 1
    if (Select-String -Path "$root\Game.log" -SimpleMatch 'LOADING: Level' -Quiet) { break }
}
Start-Sleep -Seconds 3
& "D:\Games\Crysis-RE\tools\prof\CrySampler2.exe" $p.Id $Seconds $Hz "$out\$Tag" $Threads
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $MaxRunSec) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "force-stopped" }
Copy-Item "D:\Games\Crysis-RE\logs\mt\crymt_frames.txt" "$out\$Tag.frames.txt" -Force -ErrorAction SilentlyContinue
Copy-Item "$root\Game\Levels\$Level\$DemoLog.log" "$out\$Tag.bench.log" -Force -ErrorAction SilentlyContinue
Select-String -Path "$out\$Tag.bench.log" -Pattern 'Average FPS|Min FPS' | ForEach-Object { $_.Line.Trim() }
Get-Content "$out\$Tag.threads.txt"
