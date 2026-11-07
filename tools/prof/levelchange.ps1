# Level change test: starts Bin64\Crysis.exe in a CryMT mode on one level, then CryMT's test tool (CRYMT_TESTCMD) runs
# "map <level>" commands one after another; reports whether the game survived and what the fault logs say.
param([string]$Mode = 'mt', [string]$Start = 'island', [string[]]$Levels = @('rescue', 'village'), [int]$IntervalSec = 60,
      [switch]$Fullscreen)
$root = 'D:\Games\Crysis'
$log = "$root\Game.log"
$t0 = Get-Date
$env:CRYMT_TESTCMD = ($Levels | ForEach-Object { "map $_" }) -join ';'
$env:CRYMT_TESTCMDSEC = "$IntervalSec"
$view = if ($Fullscreen) { @('+r_Fullscreen', '1') } else { @('+r_Fullscreen', '0', '+r_Width', '1280', '+r_Height', '720') }
$p = Start-Process -FilePath "$root\Bin64\Crysis.exe" -WorkingDirectory "$root\Bin64" -ArgumentList (@('-dx10', '-DEVMODE', "-crymt=$Mode") + $view + @('+map', $Start)) -PassThru
$env:CRYMT_TESTCMD = $null; $env:CRYMT_TESTCMDSEC = $null
$limit = 60 + $IntervalSec * ($Levels.Count + 1)
while (((Get-Date) - $t0).TotalSeconds -lt $limit -and -not $p.HasExited) { Start-Sleep -Seconds 3 }
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
"survived=$alive after $([int]((Get-Date) - $t0).TotalSeconds)s exit=$(if (-not $alive) { '0x{0:X}' -f $p.ExitCode })"
Select-String -Path $log -Pattern 'LOADING: Level|CRASH DETECTED' | ForEach-Object Line
$focus = "$root\CryMT\logs\mt\crymt_focus.txt"
if (Test-Path $focus) { Get-Content $focus | Select-String 'test command' | Select-Object -Last ($Levels.Count) | ForEach-Object Line }
$fault = "$root\CryMT\logs\mt\crymt_fault.txt"
if ((Test-Path $fault) -and (Get-Item $fault).LastWriteTime -gt $t0) {
    $c = @(Get-Content $fault); $i = ($c | Select-String '^20\d\d-' | Select-Object -Last 1).LineNumber
    '--- crymt_fault (last entry)'; $c[($i - 1)..([Math]::Min($c.Count - 1, $i + 16))]
}
