# How CryMT works

All addresses are for Crysis 1.2.1, 64-bit, build 6156. CryMT checks this build by time stamp, image size and load address of the four modules it patches: CrySystem, CryRenderD3D10, Cry3DEngine and CryAnimation. On anything else it switches itself off before patching and writes the reason to `CryMT\logs\mt\crymt_off.txt`.

## Loading

- `Bin64\Crysis.exe` (c1-launcher v8) imports `dbghelp.dll`. That DLL is not a KnownDLL, so `Bin64\dbghelp.dll`, CryMT's proxy, loads before any engine module.
- The proxy reads `CryMT\CryMT.ini`, sets the `CRYMT_*` environment and loads `CryMT.dll`. Every real dbghelp call is forwarded to the system copy.
- c1-launcher v8 is required for two reasons:
  - It exports `CryMalloc`/`CryFree`, which CryMT patches to defer frees made during a frame.
  - Its pool keeps the engine's node allocator below 4 GB. That allocator stores pointers through 32-bit moves, the 64-bit crash of the original game.

## Threads

```
main thread:    [game update N+1: AI, physics, entities]  [scene traversal N+1 -> render lists]
render thread:  [frame N: list sort -> water reflection -> shadow maps -> z-prepass -> general -> transparent -> post]
D3D12 thread:   [the render thread's command stream -> D3D12 command lists -> GPU]
workers:        pipeline state creation, save writing
```

**The render thread.** The renderer's `EF_EndEf3D`, which draws the frame, runs on CryMT's render thread.

**The fill window.** While a frame is drawn, the main thread already fills the next one. About 1,700 renderer methods are wrapped by thunks. Each call is queued, answered from a snapshot taken at the frame boundary (view, camera, time, skeleton poses), or waits for the render thread.

**What a 2007 single-threaded engine needed for this:**
- engine garbage collection deferred to the next frame;
- fences before the light and node updates;
- character poses copied on main;
- materials released only on main;
- terrain sector meshes rebuilt instead of edited in place;
- level changes made synchronous.

**The barrier.** Water reflections and the shadow casters' fill call back into the 3D engine from inside the renderer. While they run, the main thread waits: about 1.2–2.7 ms a frame. This is the main remaining coupling.

## D3D layer and the D3D12 back end

**Wrapper and stream.** The renderer's final D3D10 device is CryMT's `ID3D10Device1`. It records the immediate context into a command stream. A stream thread executes that stream on CryMT's D3D12 implementation (`D3D12Backend.cpp`).

**Translation.**
- One root signature with root CBVs and per-stage SRV/sampler tables.
- The engine's DXBC shaders go straight into pipeline state objects.
- Buffers are suballocated from default-heap pages, and dynamic data comes from upload rings.
- Barriers are tracked per resource.
- The swap chain is flip-model.

**Pipeline states.** Every pipeline used is saved as a recipe in `CryMT\cache\d3d12_psos.bin`. At start, worker threads create all known ones. A pipeline not seen before is created in the background, and its object draws are skipped until it is ready.

**Address space.** The D3D12 driver is created before Cry3DEngine loads. So the address ranges of the game modules not loaded yet are reserved first, and each is released when CrySystem loads that module. CryMT patches those modules at fixed addresses.

## Own passes

The engine merges only copies of one mesh chunk into a batch, so nearly every object is its own batch. Each batch repeats a full shader setup of about 3,700 cycles. The D3D runtime and driver are a small part of the frame (1.3–2 ms); the engine's per-object code is the cost.

**z-prepass** (`ZPass.cpp`, `CRYMT_ZOWN` and related variables).
- For each batch the engine would make, the engine draws the first object.
- The render thread captures about 16 bytes per further object and records one group command.
- The D3D12 thread computes the per-instance constants (object matrix, bending, alpha test, ambient) with code verified bit for bit against the engine's and expands the group into draws.
- Objects that differ only in a texture the z-shader never reads join the batch.
- On the harbor level 85–87% of z-prepass objects take this path, and the pass goes from 1.62 to 0.62 ms of render-thread time.

**General pass** (`CRYMT_TEXRELAX`). Objects of one mesh that differ only in their per-object texture are drawn as one batch, with the texture rebound per run. This takes about 1 ms off the render thread on harbor.

**Not yet ours:** shadow maps, reflections, transparent objects and post effects use the engine's own path.

## DLAA

- Through NVIDIA Streamline, with manual hooking.
- The camera jitter goes into the engine's projection, Halton(2,3) with 16 phases.
- Camera motion comes from depth and the unjittered matrices.
- DLAA runs before the first HDR post-processing list. Its result replaces the HDR target, and same-size copies the engine made earlier also get the anti-aliased image.

## Global illumination

`CryMTSettings.exe --gi-install` patches the player's own shader sources from `Game\Shaders.pak` into `Game\zzCryMT.pak`: fragLib, Terrain, ShadowMaskGen, CommonZPass and Particles. Small placeholder textures in that pak are swapped by the back end for its own results:

- the GI term, sampled in the ambient;
- a frame texture with the jitter, sun and camera;
- a foliage mask written by the z-pass;
- the particle light volume.

The back end runs:
- voxel grids of 4 levels (0.25 to 2 m cells) around the camera, filled from the depth buffer and from the z-prepass geometry;
- cone tracing through them for occlusion and bounced light;
- visibility-bitmask screen-space GI;
- GTAO in place of the engine's SSAO;
- contact shadows;
- two 64×64×32 light volumes for blended particles.

## Launcher

`CryMTLauncher.exe` is a frameless WebView2 window. Its page (`src/launcher/ui`) does all settings work:

- `options.js` describes every setting: its ini key, a codec that reads the value the way the DLL does, its hint, cost and hotkey.
- `ini.js` merges values into `CryMT.ini`, changing only the lines whose values changed.

The window reads and writes files, starts the game, and installs updates:

1. It reads the release list from GitHub.
2. It checks the signature of `manifest.txt` (ECDSA P-256, public key in `UpdateKey.h`).
3. It checks the SHA-256 of every file in the package.
4. It backs up the files it replaces, and replaces the running exe through a rename.
5. "Rollback" restores the backup.

## Debugging aids

| Tool | What it gives |
|---|---|
| `CRYMT_RENDERDOC=1` | loads RenderDoc before the device; PgUp captures a frame with named passes |
| `tools/prof/rdc_summary.py` | summarizes a capture through RenderDoc's Python |
| `CRYMT_GPUPROF` | GPU timestamps per render list |
| `CRYMT_ZCYCLES` | cycle counts of the per-object path |
| `*VERIFY` switches | run the engine's path next to CryMT's and compare results |
| capture key and `CRYMT_CAPTURE_*` | frames as images; `=toggle` variants alternate CryMT's path and the engine's on odd and even frames |
