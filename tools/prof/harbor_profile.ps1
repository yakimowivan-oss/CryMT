# The play mode on harbor's autotest through the dbghelp loader (-crymt=dlaa, GI as given), 3840x1080 window (32:9 like the
# user's screen, CPU bound), with CryMT's per-frame split (CRYMT_FRAMELOG=1: crymt_detail.txt, crymt_frames.txt); -Sample
# also attaches the 200 Hz sampler (distorts main: take the thread split from a run without it).
#   powershell -File tools\prof\harbor_profile.ps1 -Tag h1 [-Gi 1] [-Sample] [-Width 3840 -Height 1080]
param([string]$Tag = 'harbor', [int]$Gi = 1, [switch]$Sample, [int]$Width = 3840, [int]$Height = 1080, [int]$MaxSec = 420,
      [int]$SampleSec = 50, [string]$Level = 'harbor', [string]$Cfg = 'crymt_q_autotest', [string]$DemoLog = 'autotest')
$g = 'D:\Games\Crysis'
$out = 'D:\Games\Crysis-RE\logs\prof3'
New-Item -ItemType Directory -Force $out | Out-Null
$logs = "$g\CryMT\logs\mt"
Remove-Item "$logs\crymt_frames.txt", "$logs\crymt_detail.txt" -ErrorAction SilentlyContinue
$bench = "$g\Game\Levels\$Level\$DemoLog.log"
$before = if (Test-Path $bench) { (Get-Content $bench).Count } else { 0 }
$env:CRYMT_FRAMELOG = '1'
$gameArgs = "-dx10 -DEVMODE -crymt=dlaa -crymt-gi=$Gi +r_Width $Width +r_Height $Height +r_Fullscreen 0 +map $Level +exec $Cfg"
$p = Start-Process "$g\Bin64\Crysis.exe" -ArgumentList $gameArgs -WorkingDirectory "$g\Bin64" -PassThru
Remove-Item Env:CRYMT_FRAMELOG
$t0 = Get-Date
if ($Sample) {
    while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 120) {
        Start-Sleep -Seconds 1
        if (Select-String -Path "$g\Game.log" -SimpleMatch 'LOADING: Level' -Quiet) { break }
    }
    Start-Sleep -Seconds 8
    & 'D:\Games\Crysis-RE\tools\prof\CrySampler2.exe' $p.Id $SampleSec 200 "$out\$Tag" 5
}
if (-not $p.WaitForExit([Math]::Max(1, $MaxSec - [int]((Get-Date) - $t0).TotalSeconds) * 1000)) { Stop-Process -Id $p.Id -Force; 'force-stopped' }
Start-Sleep -Seconds 2
Copy-Item "$logs\crymt_frames.txt" "$out\$Tag.frames.txt" -Force -ErrorAction SilentlyContinue
Copy-Item "$logs\crymt_detail.txt" "$out\$Tag.detail.txt" -Force -ErrorAction SilentlyContinue
Copy-Item "$logs\d3d12.txt" "$out\$Tag.d3d12.txt" -Force -ErrorAction SilentlyContinue
@(Get-Content $bench -ErrorAction SilentlyContinue | Select-Object -Skip $before) | Select-String 'Average FPS|Min FPS' | ForEach-Object { $_.Line.Trim() }
if ($Sample -and (Test-Path "$out\$Tag.threads.txt")) { Get-Content "$out\$Tag.threads.txt" }
