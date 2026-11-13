#pragma once
#include <atomic>

// Live switches for testing and videos (CRYMT_GI_KEYS=1, the launcher's in-game keys): CryMT.cpp reads the numpad on
// main and passes it to the panel (D3D12Backend.cpp, Device::PanelKey and its item table), which writes these; the
// D3D12 back end applies them at present (scales on top of the ini values) and draws the panel. A -1 below means "as the
// ini / environment says" until the panel or the back end sets it.
struct GiControl
{
    std::atomic<bool> active{false};
    std::atomic<bool> enabled{true}, panel{true}, fastTime{false};
    std::atomic<int> parts{-1};         // 0 both GI parts, 1 the voxel cones only; -1 as CRYMT_GI_SCREEN says
    std::atomic<bool> weapon{true};     // GI on the first-person view (weapon, hands, held objects)
    std::atomic<int> view{0}; // voxel debug view: 0 off, 1 wire, 2 solid
    std::atomic<int> geometry{-1}; // voxel geometry: 0 off, 1 on, 2 with foliage as partly open
    std::atomic<int> gtao{-1};     // GTAO in place of the engine's SSAO 1, the SSAO 0
    std::atomic<int> contact{-1};  // contact shadows: 0 off, 1 the old 12 steps, 2 Bend's screen-space shadows
    std::atomic<int> sssReach{-1}; // the screen-space shadows' reach (metres); -1 as CRYMT_SSS_REACH says
    std::atomic<int> sssWeapon{-1}; // their thickness on the weapon and hands, the panel's list index; -1 as CRYMT_SSS_WEAPONTHICK
    std::atomic<int> giNear{-1};    // the GI at full strength up to (out by 2.5 x, the baked occlusion in): the panel's list
                                    // index (30 40 50 62 m); -1 as CRYMT_GI_TERRAINAONEAR
    std::atomic<int> dirLight{-1};  // the voxels' light: 0 one colour, 1 colour + direction; -1 as CRYMT_GI_DIRLIGHT
    std::atomic<int> particles{-1}; // blended particles (smoke, dust) take the GI 1 / 0
    std::atomic<int> fixCopies{1}; // DLSS's picture into the copies of the scene made before it this frame
    std::atomic<int> dlssPreset{-1}; // the DLAA model, the panel's list index (K J L M F E); -1 as CRYMT_DLSS_PRESET
    std::atomic<int> dlssCompare{-1}; // a second DLAA model for the right half of the screen (same index), -1 off
    std::atomic<int> particleOut{-1}; // their darkening outdoors, percent of the indoor one (50 70 100 150 200)
    std::atomic<int> bakedOff{-1}; // the engine's baked occlusion the GI replaces (bits 2 the objects' RAM, 4 their sky
                                   // amount, 8 TerrainAO, 16 the terrain's vertex AO)
    std::atomic<float> occlusionScale{1.0f}, bounceScale{1.0f};
    std::atomic<float> timeOfDay{-1.0f}; // the time of day as main last read it (hours), -1 unknown
    std::atomic<float> startTime{11.5f}; // the time tab's default (CRYMT_GI_STARTTIME, else 11:30)
    // The panel: its tab (GI, voxels, reflections, engine, time) and the selected row on each; NumLock off (the numpad
    // then sends arrows: the panel says so).
    std::atomic<int> tab{0};
    std::atomic<int> rows[8] = {};
    std::atomic<bool> numLockOff{false};
    std::atomic<int> gsmCache{-1};     // e_gsm_cache (far shadow cascades cached) as main last read it (a float cvar)
    std::atomic<int> gsmDefault{-1};   // ... as the loader / launcher set it at the start
    std::atomic<int> fplus{-1};        // the Forward+ test 1 / 0 (no panel row: the single-pass mode it needs is broken)
    std::atomic<int> voxOpacity{-1};   // the cells' opacity 1 from the voxelized geometry only (the screen gives their
                                       // colour), 0 also from the screen; -1 as CRYMT_GI_OCCUPANCY says
    std::atomic<int> geomCache{-1};    // unchanged objects voxelized every 4th frame 1, every frame 0; -1 as CRYMT_GI_GEOMCACHE
    std::atomic<int> env{-1};          // environment maps 1 from the voxel grid, 0 the game's; -1 as CRYMT_ENVMAP says
    std::atomic<bool> envAll{true};    // the reflection on every material (fragLib), not only those with their own map
    std::atomic<int> waterCube{-1};    // the ocean's reflection: 0 planar, 1 the environment cube, 2 SSR + the cube (the
                                       // planar pass off for 1 and 2); -1 as CRYMT_WATERCUBE says
    std::atomic<float> waterSky{-1.0f}; // the cube's brightness on the water (where SSR misses: the sky); -1 as CRYMT_WATERSKY
    std::atomic<int> waterSource{-1};   // the water's cube: 1 the engine's own capture at the camera (CryMT.cpp EnvCubeUpdate),
                                        // 0 the voxel grid's; -1 as CRYMT_ENVCUBE says
    std::atomic<int> envCubeRate{-1};   // the engine's cube: frames per face drawn (1..8); -1 as CRYMT_ENVCUBE_RATE says
    std::atomic<int> envCubeCull{-1};   // ... its faces' culling: 0 as the engine sets it, 1 none, 2 the other side; -1 as
                                        // CRYMT_ENVCUBE_CULL says
    std::atomic<int> envCubeMip{-1};    // ... the mip level the water reads (debug), -1 as the hardware picks
    std::atomic<int> envCubeBlend{-1};  // ... a new face mixed in: 0 replaces, 1 half, 2 a quarter; -1 as CRYMT_ENVCUBE_BLEND
    std::atomic<int> envCubeDown{-1};   // ... its -Z face drawn 1 / left out 0 (the water looks up); -1 as CRYMT_ENVCUBE_DOWN
    std::atomic<int> envCubeEntities{-1}; // ... entities in it (clouds, vehicles, characters) 1 / 0; -1 as CRYMT_ENVCUBE_ENTITIES
    std::atomic<float> envOwnScale{1.0f}, envAllScale{1.0f}; // their strengths
};
extern GiControl g_giControl;

// The panel's keys (main thread, Device::PanelKey).
enum PanelAction
{
    kPanelShow, // Num/: show or hide
    kPanelTabPrev, kPanelTabNext, // Num7 / Num9
    kPanelUp, kPanelDown,         // Num8 / Num2
    kPanelLess, kPanelMore,       // Num4 / Num6
    kPanelDefault,                // Num5
};
void Dx12PanelKey(int action);
