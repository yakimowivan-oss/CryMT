# Building CryMT

Everything builds with **llvm-mingw** (clang 23, x86_64-w64-mingw32) on Windows, without Visual Studio.

## 1. Toolchain and SDK headers

Put them into the repository folder:

| Folder | What | Where from |
|---|---|---|
| `tools/llvm` | llvm-mingw (ucrt, x86_64), its `bin` with `x86_64-w64-mingw32-clang++.exe` | https://github.com/mstorsjo/llvm-mingw/releases |
| `sdk/streamline-v2.14.16` | NVIDIA Streamline SDK 2.14.16 (headers in `include`) | https://github.com/NVIDIA-RTX/Streamline/releases |
| `sdk/webview2/pkg` | Microsoft.Web.WebView2 1.0.2903.40, the NuGet package unzipped (`build/native/include`, `build/native/x64/WebView2Loader.dll`) | https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.2903.40 |
| `sdk/renderdoc` | `renderdoc_app.h` from RenderDoc 1.47 | https://renderdoc.org |
| `streamline` | runtime DLLs for packages: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` + `nvngx_dlss.license.txt` | the Streamline SDK's `bin/x64` |

## 2. Build

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

This builds `build\CryMT.dll`, `dbghelp.dll`, `CryMTSettings.exe`, `CryMTReport.exe`, `CryMTLauncher.exe`, `CryMTSign.exe` and `Inject.exe`. Use `-Only CryMT,Launcher` to build some of them.

## 3. Install for testing

Copy the build outputs into a Crysis 1.2.1 64-bit folder that has c1-launcher v8 as `Bin64\Crysis.exe`:

1. `dbghelp.dll` goes into `Bin64\`.
2. These go into `CryMT\`:
   - `CryMT.dll`, `CryMTSettings.exe`, `CryMTReport.exe`, `CryMTLauncher.exe`;
   - `sdk/webview2/pkg/build/native/x64/WebView2Loader.dll`;
   - `src/launcher/ui` as `CryMT\ui`;
   - the Streamline runtime as `CryMT\streamline`.

For working on the launcher page without copying it, start it as `CryMTLauncher.exe --ui=<repo>\src\launcher\ui --dev`. In that mode F5 reloads the page and F12 opens the developer tools.

## 4. Checks

`node tools/prof/launchercheck.js [CryMT.ini]` checks the launcher against the code:

- every `CRYMT_*` variable the DLL reads is either described in `src/launcher/ui/options.js` or listed as hidden on purpose;
- number defaults match the code;
- an unchanged save leaves `CryMT.ini` byte for byte.

## 5. Releases

`tools\release\make_release.ps1 -Version <N> [-Test] [-Publish]` does the following:

1. builds everything;
2. stages the package with paths as they are in the game folder;
3. writes `manifest.txt` with the SHA-256 of every file and signs it with `keys\crymt-release.key`;
4. zips the package and re-checks every file in the zip;
5. with `-Publish`, creates the GitHub release in CryMT-Releases.

The signing key is not in the repository. The launcher accepts only manifests signed with it: the public half is `src/launcher/UpdateKey.h`. A fork that publishes its own builds makes its own key with `CryMTSign keygen` and replaces that header.
