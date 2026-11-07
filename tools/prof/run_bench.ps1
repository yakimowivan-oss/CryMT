param(
    [string]$Tag = "baseline",
    [string]$Level = "island",
    [string]$Script = "benchmark_cpu",
    [int]$LoadWaitSec = 120,
    [int]$SampleSec = 60,
    [int]$Hz = 100,
    [int]$MaxRunSec = 200,
    [string[]]$Extra = @(),
    [string]$Bin = "Bin64",
    [switch]$NoSample,
    [switch]$Mt,
    [int]$ShotSec = 0,
    [int]$Shots = 1
)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class Shot {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
  [DllImport("user32")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32")] static extern bool ClientToScreen(IntPtr h, ref Point p);
  [DllImport("user32")] static extern bool SetForegroundWindow(IntPtr h);
  public static string Save(IntPtr hwnd, string path) {
    if (hwnd == IntPtr.Zero) return "no window";
    SetForegroundWindow(hwnd);
    System.Threading.Thread.Sleep(400);
    RECT r; GetClientRect(hwnd, out r);
    Point p = new Point(0, 0); ClientToScreen(hwnd, ref p);
    using (var bmp = new Bitmap(r.R, r.B)) {
      using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(p.X, p.Y, 0, 0, bmp.Size);
      bmp.Save(path, ImageFormat.Png);
    }
    return path;
  }
}
"@
$root = "D:\Games\Crysis"; $out = "D:\Games\Crysis-RE\logs\prof"; New-Item -ItemType Directory -Force $out | Out-Null
$log = "$root\Game.log"; $bench = "$root\Game\Levels\$Level\$Script.log"
if (Test-Path $bench) { Remove-Item $bench }
$gameArgs = @('-dx10','-DEVMODE','+r_Fullscreen','0','+r_Width','1280','+r_Height','720') + $Extra + @('+map',$Level)
if ($Script) { $gameArgs += @('+exec',$Script) }
if ($Mt) {
    $build = if ($env:CRYMT_BUILD) { $env:CRYMT_BUILD } else { "D:\Games\Crysis-RE\src\crymt\build" }
    # Render thread unless the caller chose a mode; frame logs for detail.py and gpuwin.py.
    $setMode = -not $env:CRYMT_MODE
    if ($setMode) { $env:CRYMT_MODE = "2"; $env:CRYMT_LEVEL = "2" }
    $env:CRYMT_FRAMELOG = "1"
    $childId = & "$build\Inject.exe" "$build\CryMT.dll" $root @gameArgs
    if ($setMode) { $env:CRYMT_MODE = $null; $env:CRYMT_LEVEL = $null }
    $env:CRYMT_FRAMELOG = $null
    if (-not $childId) { "injector failed"; exit 1 }
    $p = Get-Process -Id ([int]$childId)
} else {
    # Bin64\dbghelp.dll loads CryMT from CryMT.ini unless told otherwise: vanilla unless the caller picked a mode.
    if (-not ($Extra | Where-Object { $_ -like '-crymt=*' })) { $gameArgs = @('-crymt=off') + $gameArgs }
    $p = Start-Process -FilePath "$root\$Bin\Crysis.exe" -WorkingDirectory "$root\$Bin" -ArgumentList $gameArgs -PassThru
}
$t0 = Get-Date; $ready = $false
while (((Get-Date)-$t0).TotalSeconds -lt $LoadWaitSec) {
    Start-Sleep -Seconds 2
    if ($p.HasExited) { break }
    if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch 'LOADING: Level' -Quiet)) { $ready = $true; break }
}
"ready=$ready after $([int]((Get-Date)-$t0).TotalSeconds)s exited=$($p.HasExited)"
if ($ready -and $ShotSec -gt 0) {
    Start-Sleep -Seconds $ShotSec
    $p.Refresh()
    for ($k = 1; $k -le $Shots; $k++) {
        $p.Refresh()
        if ($p.HasExited) { "shot: process exited"; break }
        $name = if ($Shots -gt 1) { "$out\$Tag`_$k.png" } else { "$out\$Tag.png" }
        "shot: " + [Shot]::Save([IntPtr]$p.MainWindowHandle, $name)
        Start-Sleep -Milliseconds 150
    }
}
if ($ready -and $Bin -eq "Bin64" -and -not $NoSample) {
    Start-Sleep -Seconds 5
    & "D:\Games\Crysis-RE\tools\prof\CrySampler.exe" $p.Id $SampleSec $Hz "$out\$Tag"
}
while (-not $p.HasExited -and ((Get-Date)-$t0).TotalSeconds -lt $MaxRunSec) { Start-Sleep -Seconds 3 }
$crashed = (Test-Path $log) -and (Select-String -Path $log -SimpleMatch 'CRASH DETECTED' -Quiet)
"exited=$($p.HasExited) crashed=$crashed runtime=$([int]((Get-Date)-$t0).TotalSeconds)s"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "force-stopped" }
Copy-Item $log "$out\$Tag.game.log" -Force
if (Test-Path $bench) { Copy-Item $bench "$out\$Tag.bench.log" -Force; "--- bench log"; Get-Content $bench -Tail 25 } else { "no bench log at $bench" }
if ($crashed) { "--- crash"; Select-String -Path $log -SimpleMatch 'Access violation' | ForEach-Object Line }
