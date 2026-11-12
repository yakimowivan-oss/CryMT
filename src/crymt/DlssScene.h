// One frame's scene for DLSS, filled by CryMT on the render thread and carried with the command stream.
#pragma once
#include <cstdint>

struct DlssSceneInfo
{
    float view[16];         // world to view, D3DX style (row vectors, rows in memory)
    float proj[16];         // view to clip without jitter (right-handed)
    float jitterX, jitterY; // pixels: how far the projection jitter moved the image (+x right, +y down)
    float nearZ, farZ, fov, aspect;
    uint32_t frame;
    uint32_t reset; // no history: first frame, level load, camera cut
    uint32_t valid; // a main camera was seen this frame
    uint32_t pad;
    float sun[8];   // xyz towards the sun, w 1 when found; rgb its colour (HDR), a unused (the GI's voxel light)
    // The lamps for the GI's voxel light (the 8 nearest the camera, not the sun), world metres: per lamp xyz position,
    // radius; rgb colour (HDR), the cosine of a projector's half angle (-2: a point light); xyz a projector's direction.
    float lamps[8][12];
    uint32_t lampCount;
    uint32_t pad2[3];
    // The VisAreas near the camera (up to 16, the nearest), world metres: per area x, y min and max, z min and max, the
    // point count, 0, then up to 16 points of its outline (x, y).
    float visAreas[16][40];
    uint32_t visAreaCount;
    uint32_t pad3[3];
    // The camera jitter this frame's main camera got (fractions of the screen, the image moved +x right, +y down; 0
    // without DLAA), set with the camera, before the frame's draws: the deferred shadows rebuild points with it.
    float shiftU, shiftV;
    uint32_t pad4[2];
    // Forward+ test: the frame's point lamps without shadow maps (not the sun, projectors or fake lights), taken at the
    // frame's first main camera, world metres: per lamp xyz position, 1 / radius; rgba colour (a the specular
    // multiplier), as the engine's light info texture has them. The camera position (w 1) and the screen for the tiles.
    float fplusLamps[32][8];
    uint32_t fplusCount;
    uint32_t fplusWidth, fplusHeight;
    uint32_t pad5;
    float fplusCam[4];
};
