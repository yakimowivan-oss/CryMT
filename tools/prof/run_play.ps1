param(
    [string]$Tag = "play",
    [string]$Dir = "D:\Games\Crysis-RE\release\v1",
    [int]$Seconds = 120,
    [int]$ShotEvery = 20,
    [string[]]$GameArgs = @('-dx10', '+r_Fullscreen', '0', '+r_Width', '1280', '+r_Height', '720', '+hud_startPaused', '0', '+map', 'island')
)
# Plays a level without the benchmark script through the CryMT render thread and takes periodic screenshots.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class PlayShot {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
  [DllImport("user32")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32")] static extern bool ClientToScreen(IntPtr h, ref Point p);
  public static void Save(IntPtr hwnd, string path) {
    RECT r; GetClientRect(hwnd, out r);
    Point p = new Point(0, 0); ClientToScreen(hwnd, ref p);
    using (var bmp = new Bitmap(r.R, r.B)) {
      using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(p.X, p.Y, 0, 0, bmp.Size);
      bmp.Save(path, ImageFormat.Png);
    }
  }
}
"@
$out = "D:\Games\Crysis-RE\logs\play"; New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "D:\Games\Crysis-RE\logs\mt\crymt_fault.txt" -ErrorAction SilentlyContinue
$env:CRYMT_MODE = "2"; $env:CRYMT_LEVEL = "2"
$id = & "$Dir\Inject.exe" "$Dir\CryMT.dll" "D:\Games\Crysis" @GameArgs
$env:CRYMT_MODE = $null; $env:CRYMT_LEVEL = $null
$p = Get-Process -Id ([int]$id)
$t = Get-Date; $k = 0
while (-not $p.HasExited -and ((Get-Date) - $t).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds $ShotEvery
    $p.Refresh()
    if ($p.HasExited) { break }
    $k++
    try { [PlayShot]::Save($p.MainWindowHandle, "$out\$Tag`_$k.png") } catch { "shot failed: $_" }
    $stats = Get-Content "D:\Games\Crysis-RE\logs\mt\crymt_stats.txt" -ErrorAction SilentlyContinue | Select-String '^frames=' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(80, $_.Line.Length)) }
    "t=$([int]((Get-Date) - $t).TotalSeconds)s cpu=$([int]$p.TotalProcessorTime.TotalSeconds)s $stats"
}
"exited=$($p.HasExited)"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
if (Test-Path "D:\Games\Crysis-RE\logs\mt\crymt_fault.txt") { Get-Content "D:\Games\Crysis-RE\logs\mt\crymt_fault.txt" | Select-Object -First 30 }
Copy-Item "D:\Games\Crysis\Game.log" "$out\$Tag.game.log" -Force
Select-String -Path "$out\$Tag.game.log" -Pattern 'CRASH DETECTED|Access violation' | ForEach-Object Line
