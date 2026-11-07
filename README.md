# CryMT

Multithreaded rendering, a DirectX 12 back end, NVIDIA DLAA and global illumination for **Crysis (2007)**, 64-bit, patch 1.2.1, build 6156.

CryMT is a runtime modification. `Bin64\dbghelp.dll` loads `CryMT.dll` whenever the game starts, and CryMT changes the engine in memory, so no game file is replaced. It was made by reverse engineering the shipped binaries. Leaked engine source code was not used.

- **Players:** download builds and the launcher from **[CryMT-Releases](https://github.com/yakimowivan-oss/CryMT-Releases)**.
- **Developers:** this repository has the source. See [BUILDING.md](BUILDING.md) to build it and [ARCHITECTURE.md](ARCHITECTURE.md) for how it works.

*Русский: CryMT — модификация движка Crysis 1.2.1 (64 бит): рендер в отдельном потоке, свой бэкенд DirectX 12, NVIDIA DLAA и глобальное освещение. Готовые сборки и лаунчер лежат в [CryMT-Releases](https://github.com/yakimowivan-oss/CryMT-Releases). Здесь — исходники.*

## What it does

- **Render thread.** Stock Crysis does game logic, scene traversal and every draw on one thread. CryMT moves drawing to its own thread, so the main thread builds frame N+1 while frame N is drawn.
- **D3D12 back end.** The engine's D3D10 calls are recorded into a command stream and executed on CryMT's own D3D12 implementation on a separate thread.
  - Pipeline states are created ahead of time from recipes of earlier runs, and new ones are created in the background.
- **Own render passes.**
  - The z-prepass draws batches of objects with per-object constants computed off the render thread.
  - The general pass merges draws the engine kept separate.
- **NVIDIA DLAA** through Streamline. It runs on the HDR scene before tone mapping, bloom and the HUD.
- **Global illumination**, added to the engine's ambient term through a small shader patch:
  - voxel grids around the camera, filled with the scene's geometry;
  - visibility-bitmask screen-space GI;
  - GTAO;
  - contact shadows;
  - lighting for smoke and dust.
- **Quality of life.**
  - Borderless fullscreen.
  - Pause when the game loses focus.
  - Saves written in the background.
  - A crash reporter.
  - A per-frame budget for procedural vegetation.
  - RenderDoc frame capture.
- **Launcher.**
  - A WebView2 window with an HTML UI that writes `CryMT.ini`.
  - Signed updates from GitHub releases, with rollback.

## Repository layout

| Path | What |
|---|---|
| `src/crymt` | `CryMT.dll`: render thread, D3D layer, D3D12 back end, DLAA, GI, own passes |
| `src/crymt/proxy` | `Bin64\dbghelp.dll`: loads CryMT at game start and forwards dbghelp to the system copy |
| `src/crymt/settings` | `CryMTSettings.exe`: patches the game's own shader sources into `Game\zzCryMT.pak` |
| `src/crymt/report` | `CryMTReport.exe`: crash reports |
| `src/launcher` | `CryMTLauncher.exe` (WebView2 host) and `ui/`, where `options.js` describes every setting |
| `src/launcher/sign` | `CryMTSign.exe`: release signing (ECDSA P-256) |
| `tools/prof` | profiling, capture and check scripts used during development (paths are the author's) |
| `tools/release` | `make_release.ps1`, package readmes, release notes |

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).

**Additional permission under GNU GPL version 3 section 7.** You may modify this Program, or any covered work, by linking or combining it with any of the following, or with modified versions of them, containing parts covered by the terms of their own licenses:

- the binaries of Crysis and CryEngine 2;
- the NVIDIA Streamline and NVIDIA DLSS runtime libraries;
- Microsoft WebView2.

The licensors of this Program grant you additional permission to convey the resulting work.

Crysis and CryEngine are trademarks of Crytek. This repository contains no game files or game source code. The shader patch holds only CryMT's own code and short anchor lines to find its place in the player's own copy of the shaders.
