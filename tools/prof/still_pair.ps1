param([string]$Mode = 'mixed', [int]$LoadWait = 55)
# Village, F2 twice (to a checkpoint), then two PgUp captures while standing still.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Keys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    public static void Press(ushort vk, ushort scan, bool extended) {
        uint ext = extended ? 1u : 0u;
        INPUT[] down = new INPUT[1]; down[0].type = 1; down[0].ki.wVk = vk; down[0].ki.wScan = scan; down[0].ki.dwFlags = 8u | ext;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(120);
        INPUT[] up = new INPUT[1]; up[0].type = 1; up[0].ki.wVk = vk; up[0].ki.wScan = scan; up[0].ki.dwFlags = 8u | 2u | ext;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
$env:CRYMT_GI_STARTTIME = '7'
$env:CRYMT_RENDERDOC = '1'
Remove-Item Env:CRYMT_RENDERDOC_AT, Env:CRYMT_RENDERDOC_AT2 -ErrorAction SilentlyContinue
if ($Mode -eq 'geometry') { $env:CRYMT_GI_OCCUPANCY = 'geometry' } else { Remove-Item Env:CRYMT_GI_OCCUPANCY -ErrorAction SilentlyContinue }
$dir = 'D:\Games\Crysis\CryMT\logs\renderdoc'
$before = @(Get-ChildItem "$dir\*.rdc" | ForEach-Object { $_.FullName })
$p = Start-Process 'D:\Games\Crysis\Bin64\Crysis.exe' -ArgumentList '-dx10 -DEVMODE +map village +exec girecord' -WorkingDirectory 'D:\Games\Crysis\Bin64' -PassThru
Start-Sleep $LoadWait
$p.Refresh()
function Focus { $p.Refresh(); [Keys]::ShowWindow($p.MainWindowHandle, 9) | Out-Null; [Keys]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 400 }
Focus; [Keys]::Press(0x71, 0x3C, $false); Start-Sleep 4
Focus; [Keys]::Press(0x71, 0x3C, $false); Start-Sleep 10
Focus; [Keys]::Press(0x21, 0x49, $true); Start-Sleep 35
Focus; [Keys]::Press(0x21, 0x49, $true); Start-Sleep 35
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep 3
$new = Get-ChildItem "$dir\*.rdc" | Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime
"$Mode : " + (($new | ForEach-Object { "$($_.Name) $([math]::Round($_.Length/1MB))MB $($_.LastWriteTime.ToString('HH:mm:ss'))" }) -join ', ')
