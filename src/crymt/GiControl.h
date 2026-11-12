#pragma once
#include <atomic>

// Live GI switches for testing and videos (CRYMT_GI_KEYS=1, Bin64\CryMT_GI_Test*.bat): CryMT.cpp reads the numpad on
// main and writes them; the D3D12 back end applies them at present (scales on top of the ini values) and draws the
// panel that shows them.
struct GiControl
{
    std::atomic<bool> active{false};
    std::atomic<bool> enabled{true}, panel{true}, fastTime{false};
    std::atomic<int> parts{-1};         // Num1: 0 both GI parts, 1 the voxel cones only; -1 as CRYMT_GI_SCREEN says
    std::atomic<bool> weapon{true};     // Num.: GI on the first-person view (weapon, hands, held objects)
    std::atomic<int> view{0}; // voxel debug view: 0 off, 1 wire, 2 solid
    std::atomic<int> geometry{-1}; // Num3, voxel geometry: 0 off, 1 on (2 with foliage as partly open, ini only); -1 until set
    std::atomic<int> gtao{-1};     // PgDn: GTAO in place of the engine's SSAO 1, the SSAO 0; -1 until set
    std::atomic<int> contact{-1};  // Home: contact shadows 1 / 0; -1 until set
    std::atomic<int> particles{-1}; // "-": blended particles (smoke, dust) take the GI 1 / 0; -1 until set
    std::atomic<int> fixCopies{1}; // "]": DLSS's picture into the copies of the scene made before it this frame
    std::atomic<int> particleOut{-1}; // "=": their darkening outdoors, percent of the indoor one (50 70 100 150 200); -1 until set
    std::atomic<int> bakedOff{-1}; // 6 8 9 0: the engine's baked occlusion the GI replaces (bits 2 the objects' RAM, 4 their
                                   // sky amount, 8 TerrainAO, 16 the terrain's vertex AO); -1 until set
    std::atomic<float> occlusionScale{1.0f}, bounceScale{1.0f};
    std::atomic<float> timeOfDay{-1.0f}; // the time of day as main last read it (hours), -1 unknown
    std::atomic<float> startTime{11.5f}; // Num6 goes back to it (CRYMT_GI_STARTTIME, else 11:30)
    // Num/ pages: 0 GI, 1 voxels, 2 environment maps, 3 engine (the numpad's switches below while it shows; the time keys
    // on all).
    std::atomic<int> page{0};
    std::atomic<int> gsmCache{-1};     // engine page Num0: e_gsm_cache (far shadow cascades cached) as main last read it
    std::atomic<int> fplus{-1};        // engine page Num1: the Forward+ test 1 / 0; -1 as CRYMT_FPLUS says
    std::atomic<int> voxOpacity{-1};   // voxels page Num0: the cells' opacity 1 from the voxelized geometry only (the screen
                                       // gives their colour), 0 also from the screen; -1 as CRYMT_GI_OCCUPANCY says
    std::atomic<int> geomCache{-1};    // voxels page Num1: unchanged objects voxelized every 4th frame 1, every frame 0; -1 as
                                       // CRYMT_GI_GEOMCACHE says
    std::atomic<int> env{-1};          // Num0: environment maps 1 from the voxel grid, 0 the game's; -1 as CRYMT_ENVMAP says
    std::atomic<bool> envAll{true};    // Num1: the reflection on every material (fragLib), not only those with their own map
    std::atomic<float> envOwnScale{1.0f}, envAllScale{1.0f}; // Num4/Num7, Num5/Num8: their strengths
};
extern GiControl g_giControl;
