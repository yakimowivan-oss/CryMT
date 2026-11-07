# Builds every CryMT binary with llvm-mingw into build\ (see BUILDING.md for the toolchain and the SDK headers).
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Only CryMT,Launcher,...] [-Out <folder>]
# Parts: CryMT (CryMT.dll), Loader (dbghelp.dll for Bin64), Settings (CryMTSettings.exe: GI shader install),
# Report (CryMTReport.exe: crash reports), Launcher (CryMTLauncher.exe), Sign (CryMTSign.exe: release signing),
# Inject (Inject.exe: development launches).
param([string[]]$Only = @(), [string]$Out = "$PSScriptRoot\build")
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = $PSScriptRoot
$cxx = "$root\tools\llvm\bin\x86_64-w64-mingw32-clang++.exe"
if (-not (Test-Path $cxx)) { throw "llvm-mingw not found at $cxx (BUILDING.md)" }
New-Item -ItemType Directory -Force $Out | Out-Null
$common = @('-std=c++17', '-O2', '-Wall', '-Wno-unknown-pragmas', '-static')
$crymt = "$root\src\crymt"
$launcher = "$root\src\launcher"
$parts = [ordered]@{
    CryMT    = @{Out = 'CryMT.dll'; Args = @('-shared', '-Wno-unused-but-set-global', '-Wno-unused-function', "-I$root\sdk\streamline-v2.14.16\include",
                                              "-I$root\sdk\renderdoc") + @('CryMT.cpp', 'D3DLayer.cpp', 'D3D12Backend.cpp', 'Dlss.cpp', 'ZPass.cpp',
                                              'Paths.cpp', 'Window.cpp', 'RenderDocCapture.cpp', 'MemoryFix.cpp' | ForEach-Object { "$crymt\$_" }) +
                                            @('-luser32', '-luuid', '-ldxgi', '-lgdi32')}
    Loader   = @{Out = 'dbghelp.dll'; Args = @('-shared', "$crymt\proxy\DbgHelpProxy.cpp", "$crymt\proxy\DbgHelpStubs.S", "$crymt\proxy\DbgHelp.def")}
    Settings = @{Out = 'CryMTSettings.exe'; Args = @('-municode', '-mwindows', "$crymt\settings\Settings.cpp", '-luser32', '-lgdi32', '-lshell32',
                                                     '-lole32', '-lcomctl32', '-lversion')}
    Report   = @{Out = 'CryMTReport.exe'; Args = @('-municode', '-mwindows', "$crymt\report\Report.cpp", '-luser32', '-lshell32')}
    Launcher = @{Out = 'CryMTLauncher.exe'; Args = @('-municode', '-mwindows', "-I$root\sdk\webview2\pkg\build\native\include", "-I$launcher",
                                                     "$launcher\Launcher.cpp", '-luser32', '-lole32', '-lshell32', '-ldwmapi', '-lversion', '-lgdi32',
                                                     '-luuid', '-lwinhttp', '-lbcrypt', '-lshlwapi')}
    Sign     = @{Out = 'CryMTSign.exe'; Args = @('-municode', "$launcher\sign\CryMTSign.cpp", '-lbcrypt')}
    Inject   = @{Out = 'Inject.exe'; Args = @('-municode', "$crymt\Inject.cpp")}
}
$failed = 0
foreach ($name in $parts.Keys) {
    if ($Only.Count -and $Only -notcontains $name) { continue }
    $p = $parts[$name]
    $target = Join-Path $Out $p.Out
    $t0 = Get-Date
    & $cxx @common @($p.Args) -o $target 2>&1 | Where-Object { $_ -notmatch '^In file included|note:' } | ForEach-Object { "  $_" }
    if ($LASTEXITCODE -ne 0) { "FAILED  $name"; $failed++; continue }
    "{0,-9} {1,-20} {2,8:N0} bytes  {3,4:N1} s" -f $name, $p.Out, (Get-Item $target).Length, ((Get-Date) - $t0).TotalSeconds
}
if ($failed) { exit 1 }
