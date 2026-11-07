#pragma once
#include <atomic>

// Live GI switches for testing and videos (CRYMT_GI_KEYS=1, Bin64\CryMT_GI_Test*.bat): CryMT.cpp reads the numpad on
// main and writes them; the D3D12 back end applies them at present (scales on top of the ini values) and draws the
// panel that shows them.
struct GiControl
{
    std::atomic<bool> active{false};
    std::atomic<bool> enabled{true}, panel{true}, fastTime{false};
    std::atomic<int> parts{0};          // Num1: 0 both GI parts, 1 the voxel cones only
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
};
extern GiControl g_giControl;
