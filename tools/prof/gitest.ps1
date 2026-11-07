# GI shader test with a guaranteed way back: puts the patched shaders and the placeholder (src\crymt\shaders\patched)
# into the game folder as loose files, runs the CPU benchmark demo with a screenshot, then removes the files and returns
# the user shader cache to the backup taken before any shader test (new files deleted, changed ones restored).
param([string]$Mode = 'dx12', [string]$Tag = 'gitest', [int]$ShotSec = 15, [switch]$Magenta, [switch]$Gi)
$game = 'D:\Games\Crysis\Game'
$patched = 'D:\Games\Crysis-RE\src\crymt\shaders\patched'
$cache = "$env:USERPROFILE\Documents\My Games\Crysis\Shaders"
$backup = 'D:\Games\Crysis-RE\backup\usershaders_before_gitest2'
if (-not (Test-Path $backup)) { 'no shader cache backup, stop'; exit 1 }
if (Get-Process Crysis -ErrorAction SilentlyContinue) { 'Crysis is running, stop'; exit 1 }
foreach ($dir in 'Shaders', 'Textures') { Copy-Item (Join-Path $patched $dir) $game -Recurse -Force }
if ($Magenta) { $env:CRYMT_GITEST = '1' }
if ($Gi) { $env:CRYMT_GI = '1' }
try {
    $out = & D:\Games\Crysis-RE\tools\prof\run_bench.ps1 -Tag $Tag -NoSample -ShotSec $ShotSec -Shots 1 -MaxRunSec 120 -Extra @("-crymt=$Mode")
    $out | Select-Object -First 3
    Select-String -Path D:\Games\Crysis\Game.log -Pattern 'compilation error|error X' | Select-Object -First 6 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) }
    $log = 'D:\Games\Crysis\CryMT\logs\mt\d3d12.txt'
    if (Test-Path $log) { Get-Content $log | Select-String 'GI ' | ForEach-Object Line }
} finally {
    $env:CRYMT_GITEST = $null
    $env:CRYMT_GI = $null
    Get-Process Crysis -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 3
    Remove-Item (Join-Path $game 'Shaders'), (Join-Path $game 'Textures') -Recurse -Force
    $removed = 0; $restored = 0
    Get-ChildItem $cache -Recurse -File | ForEach-Object {
        $orig = Join-Path $backup $_.FullName.Substring($cache.Length + 1)
        if (-not (Test-Path $orig)) { Remove-Item $_.FullName -Force; $removed++ }
        elseif ((Get-FileHash $orig).Hash -ne (Get-FileHash $_.FullName).Hash) { Copy-Item $orig $_.FullName -Force; $restored++ }
    }
    "cleanup: loose files removed, shader cache: $removed new files removed, $restored restored"
}
