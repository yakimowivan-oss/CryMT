# Alt+Tab test: starts Bin64\Crysis.exe fullscreen in a CryMT mode (-crymt=<mode>), waits for the level, switches
# away and back with Alt+Tab twice, then reports whether the game survived and what the fault logs say.
param([string]$Mode = 'mt', [int]$Cycles = 2, [string[]]$Extra = @())
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Kb {
  [DllImport("user32")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int index);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public static string State(IntPtr h) {
    RECT r; GetWindowRect(h, out r);
    return "foreground=" + (GetForegroundWindow() == h) + " topmost=" + ((GetWindowLongPtrW(h, -20).ToInt64() & 8) != 0) + " minimized=" + IsIconic(h) + " rect=" + r.L + "," + r.T + " " + (r.R - r.L) + "x" + (r.B - r.T);
  }
  public static void AltEnter() {
    keybd_event(0x12, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    keybd_event(0x0D, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    keybd_event(0x0D, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    keybd_event(0x12, 0, 2, UIntPtr.Zero);
  }
  public static void AltTab() {
    keybd_event(0x12, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    keybd_event(0x09, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    keybd_event(0x09, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(60);
    keybd_event(0x12, 0, 2, UIntPtr.Zero);
  }
}
"@
$root = 'D:\Games\Crysis'
$log = "$root\Game.log"
$t0 = Get-Date
$args0 = @('-dx10', '-DEVMODE', "-crymt=$Mode", '+r_Fullscreen', '1') + $Extra + @('+map', 'island')
$p = Start-Process -FilePath "$root\Bin64\Crysis.exe" -WorkingDirectory "$root\Bin64" -ArgumentList $args0 -PassThru
$ready = $false
while (((Get-Date) - $t0).TotalSeconds -lt 120 -and -not $p.HasExited) {
    Start-Sleep -Seconds 2
    if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -SimpleMatch 'LOADING: Level' -Quiet)) { $ready = $true; break }
}
"ready=$ready after $([int]((Get-Date) - $t0).TotalSeconds)s"
Start-Sleep -Seconds 12
Add-Type -AssemblyName System.Drawing
function Shot([string]$name) {
    $b = New-Object System.Drawing.Bitmap 5120, 1440
    $g = [System.Drawing.Graphics]::FromImage($b); $g.CopyFromScreen(0, 0, 0, 0, $b.Size)
    $s = New-Object System.Drawing.Bitmap $b, 1280, 360
    $s.Save("D:\Games\Crysis-RE\logs\prof\alttab_${Mode}_$name.png"); $g.Dispose(); $b.Dispose(); $s.Dispose()
    "screenshot: logs\prof\alttab_${Mode}_$name.png"
}
$p.Refresh(); $h = $p.MainWindowHandle
"start: $([Kb]::State($h))"
for ($i = 1; $i -le $Cycles -and -not $p.HasExited; $i++) {
    [Kb]::AltTab(); Start-Sleep -Seconds 5
    $p.Refresh(); "after Alt+Tab out $i : alive=$(-not $p.HasExited) $([Kb]::State($h))"
    if ($i -eq 1) { Shot 'out' }
    [Kb]::AltTab(); Start-Sleep -Seconds 8
    $p.Refresh(); "after Alt+Tab back $i : alive=$(-not $p.HasExited) $([Kb]::State($h))"
    if ($i -eq 1) { Shot 'back' }
}
for ($i = 1; $i -le 2 -and -not $p.HasExited; $i++) {
    [Kb]::AltEnter(); Start-Sleep -Seconds 6
    $p.Refresh(); "after Alt+Enter $i : alive=$(-not $p.HasExited) $([Kb]::State($h))"
}
if (-not $p.HasExited) {
    Add-Type -AssemblyName System.Drawing
    $b = New-Object System.Drawing.Bitmap 5120, 1440
    $g = [System.Drawing.Graphics]::FromImage($b); $g.CopyFromScreen(0, 0, 0, 0, $b.Size)
    $s = New-Object System.Drawing.Bitmap $b, 1280, 360
    $s.Save("D:\Games\Crysis-RE\logs\prof\alttab_$Mode.png"); $g.Dispose(); $b.Dispose(); $s.Dispose()
    "screenshot: logs\prof\alttab_$Mode.png"
}
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
"survived=$alive exit=$(if (-not $alive) { $p.ExitCode })"
$fault = "$root\CryMT\logs\mt\crymt_fault.txt"
if ((Test-Path $fault) -and (Get-Item $fault).LastWriteTime -gt $t0) { '--- crymt_fault'; Get-Content $fault -TotalCount 14 }
if (Select-String -Path $log -SimpleMatch 'CRASH DETECTED' -Quiet) { '--- Game.log crash'; Select-String -Path $log -Pattern 'Exception|Access violation|at 0x' | Select-Object -First 6 | ForEach-Object Line }
