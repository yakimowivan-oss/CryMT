// CryMT: the z-prepass geometry voxelized into the grids (RunVoxelGeometry).
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kVoxGeomShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
cbuffer VoxGeom : register(b13)
{
    float4 grid;  // xyz the grid's first cell, w cells per axis
    float4 job;   // x the axis projected along, y the frame stamp, z the cell's opacity (1 solid, less for foliage)
};
RWTexture3D<uint> occupancy : register(u0);

void voxelize(float4 pos : SV_Position)
{
    int n = (int)grid.w;
    int3 uvw = int3((int)pos.x, (int)pos.y, (int)(pos.z * n));
    // The first and last slice along the projection are left out: vertex shaders that pin depth to a plane (far
    // geometry and sky at the far plane, shadow pancaking at the near) put whole faces there, a wall of cells at the
    // grid's edge (the user: a cube like a sky dome around the camera in the Num2 view, walls at the voxels' reach).
    if (any(uvw < 0) || any(uvw >= n) || uvw.z == 0 || uvw.z == n - 1)
        return;
    int axis = (int)job.x;
    int3 local = axis == 0 ? int3(uvw.z, uvw.x, uvw.y) : axis == 1 ? int3(uvw.x, uvw.z, uvw.y) : uvw;
    int3 cell = (int3)grid.xyz + local;
    uint old;
    InterlockedMax(occupancy[(cell % n + n) % n], (uint)job.y * 256 + (uint)(saturate(job.z) * 255), old); // the most opaque of a frame
}
)CRYMT"
