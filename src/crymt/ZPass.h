// The renderer's z-prepass, replaced piece by piece (first: a census of what the original pass binds).
#pragma once

namespace zpass
{
// When CryRenderD3D10.dll is loaded. CRYMT_ZCENSUS=1: count the shaders, constant buffer slots and parameter types the
// z-prepass sets, written to logs\mt\zpass_census.txt by WriteStats.
void Install();
void OnEngineLoaded(); // Cry3DEngine.dll mapped: its hooks
void WriteStats();
uint64_t BendBeforeCount(); // the previous vegetation bending written for the z-prepass (motion vectors)
void BendBeforeStats(char* text, size_t size);
float MotionBlurShutter(); // the engine's motion blur shutter factor f last written (exact object motion), 0 none yet
void MotionBlurStats(char* text, size_t size);
} // namespace zpass
