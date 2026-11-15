// CryMT: the GI: trace, filter, motion, upsample, GTAO, probes and the rest (RunGi).
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kGiShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
cbuffer GiConstants : register(b0)
{
    float2 fullSize; float2 giSize;
    float2 invProj;     // 1 / proj._11, 1 / proj._22
    float2 depthParams; // proj._33, proj._43 (right-handed): distance = _43 / (d + _33)
    float radius; float bounce; float occlusion; float temporal; // radius..occlusion: 0 in trace (its screen part's, removed
                                                                 // 2026-11-15); the motion, GTAO and exposure passes' own
    float maxDistance; float frame; float debug; float options; // options: 1 fixed sample pattern, 2 nearest depth of the 2x2 block, 4 unused,
                                                                 // 8 narrow normals, 16 the first-person view takes the camera's GI (upsample t2),
                                                                 // 32 unused, 64 opacity per axis,
                                                                 // 128 the roof texture (CRYMT_GI_ROOFCELLS), 256 thin features
                                                                 // take their surroundings' GI (CRYMT_GI_THIN, on); upsample:
                                                                 // 512 the GI replaces the engine's SSAO (GTAO), 1024 GTAO's
                                                                 // own history, 2048-16384 the baked terms replaced, 32768
                                                                 // GTAO at full size, 65536 x half TerrainAO's GI distance
                                                                 // (5 bits), 2^21 the blended particles' GI,
                                                                 // 2^22 foliage occludes nothing in GTAO, 2^23 GTAO on
                                                                 // foliage in the sun's shadow at 0.2 (all exact in a float)
    row_major float4x4 reproj; // this frame's view space (x, y, -distance) to the previous frame's clip space
    float4 motionParams;       // motion: x far plane, y 1 with the engine's object velocities in t1; GI: z the ambient
                               // floor (Strengthen)
    float4 toWorldX; float4 toWorldY; float4 toWorldZ; // the engine's view (x, y, -distance, 1) dotted with these: world
    float4 voxGrid;   // xyz the grid's first cell, w cells per axis
    float4 voxParams; // x cell size (m), y 0 no grid (no GI) / 1 the cones, z cone reach (m), w strength
    float4 voxParams2; // x occlusion strength of the cones; near occlusion cones: y lift, z start (cells of level 0),
                       // w strength (0: none)
    float2 occRange;   // x: the cones' occlusion counts fully up to x / 2 metres, fading to none at x (light from all the
                       // reach); y: the occlusion strength where there is cover above (Strengthen)
};
// trace: t0 depth, t1 colour, u0 raw. filter: t0 depth, t1 raw, t2 previous GI, u0 new GI, u1 distance out, u2 previous.
Texture2D<float> depthTex : register(t0);
Texture2D<float4> srcTex : register(t1);
Texture2D<float4> historyTex : register(t2);
RWTexture2D<float4> outTex : register(u0);
RWTexture2D<float> distOut : register(u1);
RWTexture2D<float> distPrev : register(u2);
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);
Texture3D<float4> voxelTex : register(t3);  // grid levels 0..3: cells of voxParams.x, twice, 4 and 8 times that
Texture3D<float4> voxelTex1 : register(t4);
Texture3D<float4> voxelTex2 : register(t5);
Texture3D<float4> voxelTex3 : register(t6);
Texture3D<float4> dirTex : register(t7);   // the levels' opacity per axis (xyz) at mips 1..4 of the grid (options & 64)
Texture3D<float4> dirTex1 : register(t8);
Texture3D<float4> dirTex2 : register(t9);
Texture3D<float4> dirTex3 : register(t10);
Buffer<float4> visAreas : register(t11); // [0].x count; per area 10: x, y box, (z min, z max, points, 0), 16 points (x, y pairs)
// After the 16 areas, [161]: GI v2's options, x 1 the precise rays (CRYMT_GI_PROBEPRECISE, the panel's "Probe rays").
static const int kProbeOptions = 161;
Texture3D<float> roofTex : register(t12); // cover above per cell of level 0 at half its resolution (RoofAt)
Texture3D<float4> l1Tex : register(t13);  // the levels' light direction g (CRYMT_GI_DIRLIGHT 1, visAreas[0].z): out towards w
Texture3D<float4> l1Tex1 : register(t14); // the colour times 1 + g.w
Texture3D<float4> l1Tex2 : register(t15);
Texture3D<float4> l1Tex3 : register(t16);
// The levels' six directions (CRYMT_GI_DIRLIGHT 2, visAreas[0].z 2): t17 + level x 6 + face (+X, -X, +Y, -Y, +Z, -Z).
Texture3D<float3> sixTex[24] : register(t17);
// GI v2 (ProbeLight, pass "probes"): the probes' atlases written this frame: light and states (t41), distances (t42).
// The probes pass writes the distances through u1 (the trace's distOut there is another entry's) and reads last frame's
// through t1.
Texture2D<float4> probeTex : register(t41);
Texture2D<float4> probeDepthTex : register(t42);
Texture2D<float> giFoliageTex : register(t43); // the z-prepass's foliage mask (1 leaves, 0.6 grass; null: none), for upsample
RWTexture2D<float4> probeDepthOut : register(u1);
// The cones' light matched to the probes' past the probe grid (the user, 2026-11-14: near the probes lit, the cones
// beyond were darker, a band in the picture). In the second table after the textures (u3). [0..2] / [3..5]: the sums of
// the probes' and the cones' light (luminance x 4096) and a count over sampled pixels where both are traced (the probe
// grid's edge), by frame parity; [6] the smoothed ratio probes / cones (float bits), [7] 1 once it is set.
RWBuffer<uint> probeMatch : register(u3);

// The VisArea near the camera that holds P (the rooms the level marks as indoors; with a margin of 0.3 m, so walls on
// their border count), else -1.
int VisAreaAt(float3 P)
{
    int count = (int)visAreas[0].x;
    [loop] for (int k = 0; k < count; k++)
    {
        int b = 1 + k * 10;
        float4 box = visAreas[b], zr = visAreas[b + 1];
        if (any(P.xy < box.xy - 0.3) || any(P.xy > box.zw + 0.3) || P.z < zr.x - 0.3 || P.z > zr.y + 0.3)
            continue;
        int n = (int)zr.z;
        bool odd = false;
        float edge = 1e9;
        [loop] for (int i = 0; i < n; i++)
        {
            int j = i + 1 == n ? 0 : i + 1;
            float4 pa = visAreas[b + 2 + i / 2], pc = visAreas[b + 2 + j / 2];
            float2 a = (i & 1) ? pa.zw : pa.xy, c = (j & 1) ? pc.zw : pc.xy;
            if ((a.y > P.y) != (c.y > P.y) && P.x < a.x + (P.y - a.y) * (c.x - a.x) / (c.y - a.y))
                odd = !odd;
            float2 e = c - a, w = P.xy - a;
            edge = min(edge, length(w - e * saturate(dot(w, e) / max(dot(e, e), 1e-6))));
        }
        if (odd || edge < 0.3)
            return k;
    }
    return -1;
}

// 1 inside one of the VisAreas near the camera, else 0.
float VisInside(float3 P) { return VisAreaAt(P) >= 0 ? 1 : 0; }

// The room the GI's cones start in (CRYMT_GI_ROOMLIGHT, visAreas[0].y): its VisArea's box (with the 0.3 m margin);
// samples outside it bring no light, they still block. Sunlit roofs and outer walls came in through the coarse mips of
// thin walls (the user, village 2026-11-13: "the sun outside lights the walls inside, they are overexposed"; the GI
// darkened the stair 52.8 -> 27.2 but its upper walls and ceiling stayed light). g_roomZ.x > g_roomZ.y: no room.
static float4 g_roomXY = 0;
static float2 g_roomZ = float2(1, 0);
void SetRoom(int k)
{
    int b = 1 + k * 10;
    g_roomXY = visAreas[b] + float4(-0.3, -0.3, 0.3, 0.3);
    g_roomZ = visAreas[b + 1].xy + float2(-0.3, 0.3);
}
// How much of a sample's footprint (radius r) lies inside the room's box: 1 wholly inside, 0.5 centred on its wall,
// 0 wholly outside; 1 without a room.
float RoomWeight(float3 X, float r)
{
    if (g_roomZ.x > g_roomZ.y)
        return 1;
    float3 inside = min(X - float3(g_roomXY.xy, g_roomZ.x), float3(g_roomXY.zw, g_roomZ.y) - X);
    return saturate(min(inside.x, min(inside.y, inside.z)) / max(2 * r, 0.25) + 0.5);
}
SamplerState linearWrap : register(s2);

static const float kBias = 0.15; // horizon elevation ignored: depth steps and normal noise (no false occlusion or light)
// The first-person weapon is drawn with its own camera into depth 0..0.1 (viewport); the world lies behind. No GI on
// it, and it is no occluder for the world.
static const float kWeaponDepth = 0.1;

float Distance(float2 uv) { return depthParams.y / (depthTex.SampleLevel(pointClamp, uv, 0) + depthParams.x); }

float3 ViewPosAt(float2 uv, float d)
{
    float z = depthParams.y / (d + depthParams.x);
    return float3(float2(uv.x * 2 - 1, 1 - uv.y * 2) * invProj * z, z);
}

float3 ViewPos(float2 uv) { return ViewPosAt(uv, depthTex.SampleLevel(pointClamp, uv, 0)); }

// The scene colour can hold NaN or infinity, and min/max turn those into the clamp value (white dots); once in the
// history they never leave (0 * NaN). Integer test: the compiler may fold float NaN tests away.
bool Finite(float4 v) { return all((asuint(v) & 0x7f800000) != 0x7f800000); }

// Depth texel centre: positions rebuilt between texels are off the surface by up to half a texel, which on slopes
// turns into moire bands of false occlusion.
float2 Snap(float2 uv) { return (floor(uv * fullSize) + 0.5) / fullSize; }

// A cone through the voxel grid from world position O along dir (half angle tangent halfTan), from distance start:
// front-to-back over the grid's mips (premultiplied colour), rgb light arriving, a share blocked. Stops at the grid's edge.
// Inside grid level l with a margin of three cells. Level 0's first cell is its last update's; the coarser levels are
// updated every 2nd, 4th, 8th frame and their first cells are taken from the camera (floor(eye / cell) - n / 2), which
// may run up to a couple of cells ahead of their contents: the margin keeps the cones off cells not yet reset.
bool InGrid(int level, float3 X, float radius)
{
    float vs = voxParams.x * (1 << level), n = voxGrid.w;
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float3 g = level == 0 ? voxGrid.xyz : floor(eye / vs) - n / 2;
    return all(X - radius > (g + 3) * vs) && all(X + radius < (g + n - 3) * vs);
}

// How many cells a footprint lies inside level l's usable region (InGrid's): over the last 8 the cones go over to the
// next level (dithered per sample, one sample as before; the temporal filter smooths it), so a level's edge shows no seam
// in the GI as the camera moves (a blend of both levels cost 0.5 ms a frame on the demo).
float EdgeCells(int level, float3 X, float radius)
{
    float vs = voxParams.x * (1 << level), n = voxGrid.w;
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float3 g = level == 0 ? voxGrid.xyz : floor(eye / vs) - n / 2;
    float3 lo = (X - radius) / vs - (g + 3), hi = (g + n - 3) - (X + radius) / vs;
    return min(min(lo.x, min(lo.y, lo.z)), min(hi.x, min(hi.y, hi.z)));
}

// The opacity per axis at grid mip (1..4): the per-axis texture's mip one lower.
float3 SampleDir(int level, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    if (level == 0)
        return dirTex.SampleLevel(linearWrap, uvw, mip - 1).xyz;
    if (level == 1)
        return dirTex1.SampleLevel(linearWrap, uvw, mip - 1).xyz;
    if (level == 2)
        return dirTex2.SampleLevel(linearWrap, uvw, mip - 1).xyz;
    return dirTex3.SampleLevel(linearWrap, uvw, mip - 1).xyz;
}

float3 SampleSix(int level, int face, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    [forcecase] switch (level * 6 + face)
    {
    case 0: return sixTex[0].SampleLevel(linearWrap, uvw, mip);
    case 1: return sixTex[1].SampleLevel(linearWrap, uvw, mip);
    case 2: return sixTex[2].SampleLevel(linearWrap, uvw, mip);
    case 3: return sixTex[3].SampleLevel(linearWrap, uvw, mip);
    case 4: return sixTex[4].SampleLevel(linearWrap, uvw, mip);
    case 5: return sixTex[5].SampleLevel(linearWrap, uvw, mip);
    case 6: return sixTex[6].SampleLevel(linearWrap, uvw, mip);
    case 7: return sixTex[7].SampleLevel(linearWrap, uvw, mip);
    case 8: return sixTex[8].SampleLevel(linearWrap, uvw, mip);
    case 9: return sixTex[9].SampleLevel(linearWrap, uvw, mip);
    case 10: return sixTex[10].SampleLevel(linearWrap, uvw, mip);
    case 11: return sixTex[11].SampleLevel(linearWrap, uvw, mip);
    case 12: return sixTex[12].SampleLevel(linearWrap, uvw, mip);
    case 13: return sixTex[13].SampleLevel(linearWrap, uvw, mip);
    case 14: return sixTex[14].SampleLevel(linearWrap, uvw, mip);
    case 15: return sixTex[15].SampleLevel(linearWrap, uvw, mip);
    case 16: return sixTex[16].SampleLevel(linearWrap, uvw, mip);
    case 17: return sixTex[17].SampleLevel(linearWrap, uvw, mip);
    case 18: return sixTex[18].SampleLevel(linearWrap, uvw, mip);
    case 19: return sixTex[19].SampleLevel(linearWrap, uvw, mip);
    case 20: return sixTex[20].SampleLevel(linearWrap, uvw, mip);
    case 21: return sixTex[21].SampleLevel(linearWrap, uvw, mip);
    case 22: return sixTex[22].SampleLevel(linearWrap, uvw, mip);
    default: return sixTex[23].SampleLevel(linearWrap, uvw, mip);
    }
}

// The light the cells send back along a cone going along dir (out towards -dir): the three faces turned to it, by dir^2.
float3 SixToward(int level, float3 X, float mip, float3 dir)
{
    float3 w2 = dir * dir;
    return w2.x * SampleSix(level, dir.x > 0 ? 1 : 0, X, mip) + w2.y * SampleSix(level, dir.y > 0 ? 3 : 2, X, mip) +
           w2.z * SampleSix(level, dir.z > 0 ? 5 : 4, X, mip);
}

float3 SampleL1(int level, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    if (level == 0)
        return l1Tex.SampleLevel(linearWrap, uvw, mip).xyz;
    if (level == 1)
        return l1Tex1.SampleLevel(linearWrap, uvw, mip).xyz;
    if (level == 2)
        return l1Tex2.SampleLevel(linearWrap, uvw, mip).xyz;
    return l1Tex3.SampleLevel(linearWrap, uvw, mip).xyz;
}

float4 SampleGrid(int level, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    if (level == 0)
        return voxelTex.SampleLevel(linearWrap, uvw, mip);
    if (level == 1)
        return voxelTex1.SampleLevel(linearWrap, uvw, mip);
    if (level == 2)
        return voxelTex2.SampleLevel(linearWrap, uvw, mip);
    return voxelTex3.SampleLevel(linearWrap, uvw, mip);
}

// The cell X lies in at mip 0, no filtering (the probes' thin rays): a trilinear sample took the neighbouring cells, so
// a probe a few centimetres over sunlit sand saw the sand in every direction, up too, and the ground lit itself through
// the probes (the user, 2026-11-14: "outside everything is overexposed"; the bounce on island's beach 3.3 x the cones').
int4 GridCell(int level, float3 X)
{
    const int n = (int)voxGrid.w;
    const int3 c = (int3)floor(X / (voxParams.x * (1 << level)));
    return int4(c & (n - 1), 0); // n = 128: a mask wraps negative cells too
}

float4 LoadGrid(int level, float3 X)
{
    const int4 c = GridCell(level, X);
    if (level == 0)
        return voxelTex.Load(c);
    if (level == 1)
        return voxelTex1.Load(c);
    if (level == 2)
        return voxelTex2.Load(c);
    return voxelTex3.Load(c);
}

float3 LoadL1(int level, float3 X)
{
    const int4 c = GridCell(level, X);
    if (level == 0)
        return l1Tex.Load(c).xyz;
    if (level == 1)
        return l1Tex1.Load(c).xyz;
    if (level == 2)
        return l1Tex2.Load(c).xyz;
    return l1Tex3.Load(c).xyz;
}

// The cone samples the finest grid level that holds it and has a mip as coarse as its diameter (mip m of level l has
// cells of voxParams.x * 2^(l + m)); past the outer level's edge it stops. A sample whose footprint still reaches the
// plane of the surface the cone starts from (Ps, Ns) counts only for the part above it: near starts would otherwise take
// the surface's own cells as light and occlusion (open sand got brighter).
float4 ConeTrace(float3 O, float3 dir, float halfTan, float start, float reach, bool plane, bool surface, float3 Ps, float3 Ns, float seed)
{
    float vs = voxParams.x;
    float3 light = 0;
    float blocked = 0, occ = 0, d = start; // blocked: what the light behind passes; occ: the occlusion handed back
    // The gap before the first sample, through level 0's cells (mip 0, half a cell a step): a wall nearer than the
    // start stops the cone there. Else a cone from a ceiling or floor next to a wall started behind the wall and
    // brought the light and openness of the other side (bright bands along the edges of rooms). Not for the near
    // cones (they start within a cell of their surface).
    if (start > vs * 1.5 && InGrid(0, O, start))
        [loop] for (float t = vs * 0.5; t < start && blocked < 0.95; t += vs * 0.5)
        {
            float a = SampleGrid(0, O + dir * t, 0).a;
            occ += (1 - blocked) * a;
            blocked += (1 - blocked) * a;
        }
    [loop] for (int i = 0; i < 32 && blocked < 0.95 && d < reach; i++)
    {
        float diameter = max(2 * d * halfTan, 2 * vs);
        float3 X = O + dir * d;
        float lod = log2(diameter / vs);
        int level = 0;
        [loop] while (level < 3 && (lod - level > 4 || !InGrid(level, X, diameter * 0.5)))
            level++;
        if (!InGrid(level, X, diameter * 0.5))
            break;
        // Within 8 cells of the level's edge the next level is blended in (it takes over at the edge).
        float edge = level < 3 && InGrid(level + 1, X, diameter * 0.5) ? saturate(EdgeCells(level, X, diameter * 0.5) / 8) : 1;
        float mip = clamp(lod - level, 1, 4);
        float4 v = SampleGrid(level, X, mip);
        // Opacity per axis (CRYMT_GI_MIPOPACITY=4): a wall stops what crosses it. From a surface (Ps, Ns) the axis along
        // its normal counts only once the sample is clear of it by half its size: a coarse cell around a cone from the
        // ground holds the ground, opaque across, which darkened open ground; the far wall or the ceiling count fully.
        float3 axes = surface ? lerp(1, saturate(2 * dot(X - Ps, Ns) / diameter - 1), Ns * Ns) : 1;
        if (((uint)options & 64) != 0)
            v.a = dot(dir * dir * axes, SampleDir(level, X, mip));
        if (edge < 1)
        {
            float mip1 = clamp(lod - level - 1, 1, 4);
            float4 w = SampleGrid(level + 1, X, mip1);
            if (((uint)options & 64) != 0)
                w.a = dot(dir * dir * axes, SampleDir(level + 1, X, mip1));
            v = lerp(w, v, edge);
        }
        // CRYMT_GI_DIRLIGHT 2: the six directions' faces turned to the cone; 1: colour x (1 - g.dir) (out towards -dir).
        if (visAreas[0].z > 1.5)
        {
            float3 c6 = SixToward(level, X, mip, dir);
            if (edge < 1)
                c6 = lerp(SixToward(level + 1, X, clamp(lod - level - 1, 1, 4), dir), c6, edge);
            v.rgb = c6;
        }
        else if (visAreas[0].z > 0.5)
        {
            float3 gl = SampleL1(level, X, mip);
            if (edge < 1)
                gl = lerp(SampleL1(level + 1, X, clamp(lod - level - 1, 1, 4)), gl, edge);
            v.rgb *= max(1 - dot(gl, dir), 0);
        }
        float above = plane ? saturate((dot(X - Ps, Ns) - diameter * 0.5) / (diameter * 0.5)) : 1;
        light += (1 - blocked) * v.rgb * above * RoomWeight(X, diameter * 0.5);
        // Distant cover (hills, forest tens of metres away) took the ambient of open ground: occlusion counts fully up
        // to occRange.x, fading out by occRange.y (Crytek's cone max length is 12 m); indoors the cover is near.
        float near = occRange.x > 0 ? saturate((occRange.x - d) / (occRange.x * 0.5)) : 1;
        occ += (1 - blocked) * v.a * above * near;
        blocked += (1 - blocked) * v.a * above;
        d += diameter * 0.6;
    }
    return float4(light, occ);
}

// Cover above a point (0 open sky .. 1 a roof) from the roof texture (kVoxShader "roof": level 0 at half its
// resolution, the share of five rays up that real geometry stops within 8 m; foliage is no roof); 0 outside level 0.
float RoofAt(float3 X)
{
    return InGrid(0, X, 0) ? roofTex.SampleLevel(linearWrap, X / (voxParams.x * voxGrid.w), 0) : 0;
}

// The same along one ray from the texel (up to 8 m: occRange.x / 2, through the finest level that holds it, a cell a
// step from a jittered start, mip 0): finds the ceiling over a doorway, where the cells half a metre off a wall did
// not (village 2250: 59 -> 48% darkening with RoofAt alone).
float RoofCover(float3 O, float3 dir, float seed)
{
    float covered = 0, d = voxParams.x * (1 + seed), reach = occRange.x > 0 ? occRange.x * 0.5 : 8;
    [loop] for (int i = 0; i < 40 && covered < 0.95 && d < reach; i++)
    {
        float3 X = O + dir * d;
        int level = 0;
        [loop] while (level < 3 && !InGrid(level, X, 0))
            level++;
        if (!InGrid(level, X, 0))
            break;
        covered += (1 - covered) * SampleGrid(level, X, 0).a;
        d += voxParams.x * (1 << level);
    }
    return covered;
}

// The occlusion strength: indoors (a VisArea, else cover above: RoofAt) Num4 / Num7 (CRYMT_GI_OCCSTRENGTH,
// occRange.y), outdoors CRYMT_GI_OUTDOOR (motionParams.w), blended by the indoor share. A plain factor of 2 that suited rooms darkened everything
// outdoors; by a point's own occlusion (a curve) it could not part the two, a bush a metre away occludes as much as a
// wall (2.0 / 3.0: hut 31.7 / 26.0, path 57.7 / 56.4 vs the plain factor 23.9 / 56.0).
// Then the ambient floor (CRYMT_GI_FLOOR, motionParams.z): occ (1 - floor occ) keeps that share of the flat ambient where
// the occlusion is full and leaves light occlusion (open ground) almost as it is - with opaque coarse cells
// (CRYMT_GI_MIPOPACITY) rooms otherwise lose all of it (Crytek's TI has a diffuse bias for the same).
float Strengthen(float occ, float indoor)
{
    float o = saturate(saturate(occ) * lerp(motionParams.w, occRange.y, indoor));
    return o * (1 - motionParams.z * o);
}
// The depth texel a GI texel stands for: the top-left one of its 2x2 block, or (options & 2) the nearest one that is not
// the weapon. With DLAA jitter a fixed texel lands on thin geometry in one frame and on what lies behind in the next, so
// the GI texel flips and loses its history; the nearest of four keeps thin geometry its own texel, and the full-size
// pass gives what lies behind the GI of its neighbours.

// Full-size pixels per GI texel each way (2 or 4: Device::GiScale).
int GiScale() { return (int)round(fullSize.x / giSize.x); }

float2 GiDepthUv(int2 gi)
{
    int f = GiScale();
    int2 p = min(gi * f, int2(fullSize) - 1);
    if ((uint)options & 2)
    {
        float best = 2;
        int2 at = p;
        [unroll] for (int j = 0; j < 4; j++)
        {
            int2 q = min(gi * f + int2(j & 1, j >> 1) * (f / 2), int2(fullSize) - 1);
            float d = depthTex.Load(int3(q, 0));
            if (d >= kWeaponDepth && d < best)
            {
                best = d;
                at = q;
            }
        }
        p = at;
    }
    return (p + 0.5) / fullSize;
}

// Environment maps from the voxel grid (CRYMT_ENVMAP=voxels), for the materials' envMapSamplerCUBE / envMapSampler and
// fragLib's CryMT_EnvSampler. u0 an atlas of 26 squares of kEnvSize (13..25: what the materials get, below), t2 the
// previous run's atlas: 0..5 the cube's faces
// +X -X +Y -Y +Z -Z (D3D order, world axes: materials look it up by the world reflection vector), 6 the 2D map the
// weapons' Metal reads by reflection.xy (wrapped: a texel stands for xy and xy - 1, both upward), 7..12 the sky per
// direction, kept between runs: where the previous frame shows the far plane in a direction it moves toward that colour,
// elsewhere it stays (a = 1 seen, 0.5 only the screen's average so far). Per direction a cone from the camera through
// the grid; what it leaves unblocked sees that sky. The result is blended with the previous run's (no jumps when the
// grid or the view changes) and brought to 0..1 as the game's own environment maps are (the materials multiply it by
// the light at the surface).
static const uint kEnvSize = 64;
static const float kEnvExposure = 0.5;
groupshared float4 envSky[64];

float3 EnvFaceDir(uint face, float2 f)
{
    float u = f.x * 2 - 1, w = f.y * 2 - 1;
    return face == 0 ? float3(1, -w, -u) : face == 1 ? float3(-1, -w, u) : face == 2 ? float3(u, 1, w) :
           face == 3 ? float3(u, -1, -w) : face == 4 ? float3(u, -w, 1) : float3(-u, -w, -1);
}

// The texel of square `first` + face for a direction (the inverse of EnvFaceDir).
int2 EnvTexel(float3 d, uint first)
{
    float3 a = abs(d);
    uint face;
    float2 uv;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x > 0 ? 0 : 1;
        uv = float2(d.x > 0 ? -d.z : d.z, -d.y) / a.x;
    }
    else if (a.y >= a.z)
    {
        face = d.y > 0 ? 2 : 3;
        uv = float2(d.x, d.y > 0 ? d.z : -d.z) / a.y;
    }
    else
    {
        face = d.z > 0 ? 4 : 5;
        uv = float2(d.z > 0 ? d.x : -d.x, -d.y) / a.z;
    }
    int2 t = clamp((int2)((uv * 0.5 + 0.5) * kEnvSize), 0, kEnvSize - 1);
    return int2((first + face) * kEnvSize + t.x, t.y);
}

float4 EnvTrace(float3 O, float3 dir, float halfTan)
{
    float vs = voxParams.x;
    float3 light = 0;
    float blocked = 0, d = vs;
    [loop] for (int i = 0; i < 48 && blocked < 0.97; i++)
    {
        float diameter = max(2 * d * halfTan, vs);
        float3 X = O + dir * d;
        float lod = log2(diameter / vs);
        int level = 0;
        [loop] while (level < 3 && (lod - level > 4 || !InGrid(level, X, diameter * 0.5)))
            level++;
        if (!InGrid(level, X, diameter * 0.5))
            break;
        float4 v = SampleGrid(level, X, clamp(lod - level, 0, 4));
        light += (1 - blocked) * v.rgb;
        blocked += (1 - blocked) * v.a;
        d += diameter * 0.7;
    }
    return float4(light, blocked);
}

// The previous frame's colour where it shows the far plane in this world direction (a = 1), else a = 0.
float4 EnvSkySeen(float3 dir)
{
    float3 v = float3(dot(dir, float3(toWorldX.x, toWorldY.x, toWorldZ.x)), dot(dir, float3(toWorldX.y, toWorldY.y, toWorldZ.y)),
                      -dot(dir, float3(toWorldX.z, toWorldY.z, toWorldZ.z)));
    if (v.z > 0.05)
    {
        float2 ndc = v.xy / (v.z * invProj);
        float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        float farPlane = depthParams.y / (1 + depthParams.x);
        if (all(uv > 0.002) && all(uv < 0.998) && Distance(uv) > farPlane * 0.9)
        {
            float4 c = srcTex.SampleLevel(linearClamp, uv, 0);
            if (Finite(c))
                return float4(c.rgb, 1);
        }
    }
    return 0;
}

float3 EnvColour(float3 dir, float3 avg, float halfTan)
{
    float3 O = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    dir = normalize(dir);
    float4 t = EnvTrace(O, dir, halfTan);
    float4 sky = historyTex.Load(int3(EnvTexel(dir, 7), 0));
    return t.rgb * voxParams.w + (1 - t.a) * (sky.a > 0 ? sky.rgb : avg);
}

[numthreads(8, 8, 1)]
void envmap(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    float2 suv = float2((gi % 8 + 0.5) / 8, (gi / 8 + 0.5) / 8 * 0.6);
    float farPlane = depthParams.y / (1 + depthParams.x);
    float4 sc = srcTex.SampleLevel(linearClamp, suv, 0);
    envSky[gi] = Distance(suv) > farPlane * 0.9 && Finite(sc) ? float4(sc.rgb, 1) : 0;
    GroupMemoryBarrierWithGroupSync();
    float4 sum = 0;
    for (uint k = 0; k < 64; k++)
        sum += envSky[k];
    uint square = id.x / kEnvSize;
    if (square > 12 || id.y >= kEnvSize)
        return;
    float4 prev = historyTex.Load(int3(id.xy, 0));
    if (!Finite(prev))
        prev = 0;
    float3 avg = sum.w > 0 ? sum.rgb / sum.w : prev.a > 0 && square >= 7 ? prev.rgb : float3(0.05, 0.05, 0.05);
    float2 f = (float2(id.x % kEnvSize, id.y) + 0.5) / kEnvSize;
    if (square >= 7)
    {
        float4 seen = EnvSkySeen(normalize(EnvFaceDir(square - 7, f)));
        outTex[id.xy] = seen.a > 0 ? float4(prev.a >= 1 ? lerp(prev.rgb, seen.rgb, 0.25) : seen.rgb, 1)
                        : prev.a >= 1 || sum.w == 0 ? prev : float4(avg, 0.5);
        return;
    }
    float3 c;
    if (square < 6)
        c = EnvColour(EnvFaceDir(square, f), avg, 0.08);
    else
    {
        c = 0;
        [unroll] for (int k = 0; k < 4; k++)
        {
            float2 xy = f - float2(k & 1, k >> 1);
            c += EnvColour(float3(xy, sqrt(saturate(1 - dot(xy, xy)))), avg, 0.25) * 0.25;
        }
    }
    c = 1 - exp(-max(c, 0) * kEnvExposure);
    float3 o = prev.a > 0 ? lerp(prev.rgb, c, 0.5) : c;
    outTex[id.xy] = float4(o, 1);
    // The textures the materials get, with the panel's strengths (reproj._11: the materials' own maps, ._12: every
    // material's): squares 13..19 (cube and 2D map), 20..25 (the cube for every material).
    outTex[uint2(id.x + 13 * kEnvSize, id.y)] = float4(o * reproj._11, 1);
    if (square < 6)
        outTex[uint2(id.x + 20 * kEnvSize, id.y)] = float4(o * reproj._12, 1);
}

// GI v2 (CRYMT_GI_PROBES=1, the GI page's "GI method", visAreas[0].w): irradiance probes in the manner of DDGI (Majercik
// et al. 2019) whose rays go through the voxel grid's cells. kProbeN probes per cascade (CascadeSpacing) around the camera,
// toroidal like the voxel levels. A probe's light (rgb, as the cones') and occlusion (a) over the sphere: an octahedral
// tile of 6 x 6 texels with a border of one (8 x 8) in an atlas of 128 x 128 tiles; the rows under them hold each probe's
// state: xyz its world probe coordinate mod 1024, w 1 + the share of its rays that hit within 0.35 m (a probe inside a
// wall), 0 never traced. Each frame a quarter of the probes, and every probe new to the grid, trace 64 rays (spherical
// Fibonacci, turned per frame) and blend them in; the others are copied (two atlases in turn: cs_5_0 cannot read a
// float4 UAV). Phase 1: no visibility test yet (light can still leak through a wall between a pixel and a probe).
static const int3 kProbeN = int3(32, 32, 16);
// Cascades (the user, 2026-11-14: blotches on ceilings with probes 1 m apart, 2-3 to a room's height): 0 the near one,
// 0.5 m apart (+-8 m, +-4 m up), 1 the outer one, 1 m (+-16 m, +-8 m up); the cones beyond. Each cascade has its own
// 1024 columns of the atlases (tiles, and its states and offsets in the rows under them).
// A far one, 2 m (+-32 m), was tried and dropped (2026-11-14): +0.4 ms, and under eaves its probes 2 m apart against
// roofs a third of that thick leaked light down at mid range; past 16 m the cones go on, their light matched to the
// probes' at the grid's edge (probeMatch).
static const int kProbeCascades = 2;
float CascadeSpacing(int c) { return 0.5 * (1 << c); }
static const float2 kProbeAtlas = float2(1024 * kProbeCascades, 1152);
// Phase 2, visibility (DDGI's): per probe the mean distance to what its rays hit and its square over the sphere, a tile
// laid out as the light's (6 x 6 and a border, 1024 x 1024 RG16F); distances clamped at 1.5 x the probe cell's diagonal,
// the rays weighted by cos^8 around the texel's direction. A pixel takes a probe by Chebyshev's bound on the chance that
// nothing stands between them (a probe behind a wall gets almost no weight). DDGI's 14 x 14 at cos^50 cost ~1.5 ms here
// (256 texels x 64 rays x pow a probe) and was undersampled by 64 rays anyway.
static const float2 kProbeDepthAtlas = float2(1024 * kProbeCascades, 1024);
static const float kProbeDepthSharpness = 8;
float ProbeMaxDistance(float sp) { return 1.5 * 1.7320508 * sp; }
groupshared float4 g_probeRay[64]; // a ray's light and occlusion
groupshared float4 g_probeDir[64]; // its direction; w its distance (clamped)
groupshared uint g_probeHot;       // a texel of the probe changed beyond its rays' noise this update

int3 ProbeOrigin(int c) { return (int3)floor(float3(toWorldX.w, toWorldY.w, toWorldZ.w) / CascadeSpacing(c)) - kProbeN / 2; }
float3 ProbePos(int3 wc, int c) { return (wc + 0.5) * CascadeSpacing(c); }
int ProbeSlot(int3 wc)
{
    int3 s = wc & (kProbeN - 1); // powers of two
    return s.x + s.y * kProbeN.x + s.z * kProbeN.x * kProbeN.y;
}
int2 ProbeTile(int slot, int c) { return int2(slot & 127, slot >> 7) * 8 + int2(1024 * c, 0); }
int2 ProbeDepthTile(int slot, int c) { return ProbeTile(slot, c); }

// A border texel's interior one in a tile of (n + 2)^2: across an edge the octahedral neighbour, flipped; corners the
// opposite corner (DDGI's border copy).
int2 OctInterior(int2 t, int n)
{
    int2 ip = t - 1;
    if (ip.x < 0 || ip.x > n - 1)
    {
        ip.x = clamp(ip.x, 0, n - 1);
        ip.y = n - 1 - ip.y;
    }
    if (ip.y < 0 || ip.y > n - 1)
    {
        ip.y = clamp(ip.y, 0, n - 1);
        ip.x = n - 1 - ip.x;
    }
    return ip;
}
int2 ProbeStateTexel(int slot, int c) { return int2(1024 * c + (slot & 127), 1024 + (slot >> 7)); }
// The probe's offset from its cell's centre (xyz, metres, within 0.45 of the spacing; DDGI's relocation): a probe under
// the ground steps up towards its surface, one by a surface steps away from it.
int2 ProbeOffsetTexel(int slot, int c) { return int2(1024 * c + 128 + (slot & 127), 1024 + (slot >> 7)); }
float3 ProbeKey(int3 wc) { return float3(wc & 1023); }

float2 OctEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 s = float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    return n.z >= 0 ? n.xy : (1 - abs(n.yx)) * s;
}

float3 OctDecode(float2 o)
{
    float3 n = float3(o, 1 - abs(o.x) - abs(o.y));
    if (n.z < 0)
        n.xy = (1 - abs(n.yx)) * float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    return normalize(n);
}

// Ray i of 64 (spherical Fibonacci), turned by a rotation picked from the frame.
float3 ProbeRayDir(uint i, float f)
{
    const float phi = 6.2831853 * frac(i * 0.618034);
    const float z = 1 - (2 * i + 1) / 64.0;
    const float r = sqrt(saturate(1 - z * z));
    const float3 d = float3(r * cos(phi), r * sin(phi), z);
    const float3 h = frac(sin(float3(12.9898, 78.233, 37.719) * (f + 1)) * 43758.5453);
    const float3 axis = normalize(h * 2 - 1 + 1e-3);
    const float a = h.z * 6.2831853, c = cos(a), sn = sin(a);
    return d * c + cross(axis, d) * sn + axis * dot(axis, d) * (1 - c);
}

// A thin ray through the grid from O along dir: front-to-back over the cells at mip 0 of the finest level that holds the
// point (level 0 up to 6 m in half cells, 1 to 12 m, 2 to 24 m, then 3, a cell a step), the cells' light as the cones
// take it (L1 / six directions); rgb light, a occlusion faded with distance as the cones' (occRange); hit where it was
// half blocked (reach if never).
float4 RayTrace(float3 O, float3 dir, float reach, float jitter, out float hit)
{
    const float vs = voxParams.x;
    float3 light = 0;
    float blocked = 0, occ = 0, t = vs * 0.5 * (0.5 + jitter);
    hit = reach;
    // Level by distance: 0 up to 3 m, 1 to 8 m, 2 to 20 m, then 3 (6 / 12 / 24 m until 2026-11-14 ~07:00: the probes the
    // relocation moved off walls skim floors and walls, a mip-3 block by a surface is never empty, and level 0's half
    // cells to 6 m made the probes pass 0.5 -> 1.9 ms). One loop a level (unrolled: the level is a constant in each, so
    // a wave's rays sample one texture, not every level's in turn as in one loop with the level chosen per step); a ray
    // that leaves a level's grid goes on in the next. (Precise rays, CRYMT_GI_PROBEPRECISE - levels to 6 / 12 / 24 m, half
    // cells to 6 m, no skip over mip 2: 2.2x the cost, no visible gain - removed 2026-11-15, tag experiments-2026-11-15.)
    const float halfTo = 1.5;
    [unroll] for (int level = 0; level < 4; level++)
    {
        const float tEnd = min(level == 0 ? 3 : level == 1 ? 8 : level == 2 ? 20 : reach, reach);
        const float cell = vs * (1 << level);
        [loop] for (int i = 0; i < 64 && blocked < 0.95 && t < tEnd; i++)
        {
            const float3 X = O + dir * t;
            if (!InGrid(level, X, 0))
                break;
            // Empty space skipped: nothing in mip 3 here steps three cells at once, nothing in mip 2 one cell. Mip 3's
            // texels are 8 cells, so a wall within 3.5 cells still weighs at least 1/16 of its share (>= 1/8) in the
            // trilinear sample; mip 2's (4 cells) keep a wall within 1.5 cells at >= 1/8 of its share. A skip of two
            // cells over mip 2 could pass a wall with no weight (the stair came out 3-5 darker at 10:00).
            if (SampleGrid(level, X, 3).a < 0.002)
            {
                t += cell * 3;
                continue;
            }
            if (SampleGrid(level, X, 2).a < 0.002)
            {
                t += cell;
                continue;
            }
            float4 v = LoadGrid(level, X); // the cell itself (GridCell)
            if (v.a > 0.004) // the cells' light direction only where there is a cell
            {
                if (visAreas[0].z > 1.5)
                    v.rgb = SixToward(level, X, 0, dir);
                else if (visAreas[0].z > 0.5)
                    v.rgb *= max(1 - dot(LoadL1(level, X), dir), 0);
            }
            // Mip 0 keeps the cells' colour as it is (mip 1 premultiplies it by the opacity; the cones start at mip 1):
            // a nearly empty cell by a surface carried its full colour, and the probes over island's beach took 4-7 of
            // light from above at an occlusion of 0.1.
            v.rgb *= v.a;
            light += (1 - blocked) * v.rgb;
            const float near = occRange.x > 0 ? saturate((occRange.x - t) / (occRange.x * 0.5)) : 1;
            occ += (1 - blocked) * v.a * near;
            blocked += (1 - blocked) * v.a;
            if (blocked > 0.5 && hit >= reach)
                hit = t;
            t += level == 0 && t < halfTo ? cell * 0.5 : cell; // half cells near the probe only
        }
        // Past this level's grid or its distance: on in the next level from here (its loop ends at once when blocked).
    }
    return float4(light, occ);
}

// One group a probe slot (128 x 128 groups a cascade, z the cascade), a thread a ray and a texel of its tile. t2 the
// previous atlas, u0 this one.
[numthreads(8, 8, 1)]
void probes(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int c = gid.z;
    const float sp = CascadeSpacing(c);
    const int slot = gid.x + gid.y * 128;
    // probeMatch: last frame's sums (the trace ran after this pass) into the smoothed ratio, this frame's cleared.
    if (gid.x == 0 && gid.y == 0 && gid.z == 0 && gi == 0)
    {
        const uint last = (((uint)frame + 1) & 1) * 3, now = ((uint)frame & 1) * 3;
        const float sumProbes = probeMatch[last], sumCones = probeMatch[last + 1], count = probeMatch[last + 2];
        float ratio = probeMatch[7] != 0 ? asfloat(probeMatch[6]) : 1;
        if (count > 32 && sumCones > 64)
            ratio = lerp(ratio, clamp(sumProbes / sumCones, 0.5, 4), probeMatch[7] != 0 ? 0.05 : 1);
        probeMatch[6] = asuint(ratio);
        probeMatch[7] = 1;
        probeMatch[now] = 0;
        probeMatch[now + 1] = 0;
        probeMatch[now + 2] = 0;
    }
    const int3 s = int3(slot % kProbeN.x, (slot / kProbeN.x) % kProbeN.y, slot / (kProbeN.x * kProbeN.y));
    const int3 origin = ProbeOrigin(c);
    const int3 wc = origin + ((s - origin) & (kProbeN - 1)); // the probe in the grid now at this slot
    const int2 st = ProbeStateTexel(slot, c), texel = ProbeTile(slot, c) + int2(tid.xy);
    const float4 state = historyTex.Load(int3(st, 0));
    const bool valid = state.w >= 1 && all(state.xyz == ProbeKey(wc)) && temporal > 0;
    const int2 ot = ProbeOffsetTexel(slot, c);
    const float4 offsetHot = valid ? historyTex.Load(int3(ot, 0)) : 0;
    const float3 offset = offsetHot.xyz;
    // The outer cascades every 8th frame, the near one every 4th; a probe whose light changed beyond its rays' noise at
    // its last update (hot: the offset texel's w, frames left) every frame (visAreas[kProbeOptions].x, CRYMT_GI_PROBEFAST).
    const bool fast = visAreas[kProbeOptions].x > 0.5;
    const float hot = fast ? offsetHot.w : 0;
    const uint period = c == 0 ? 3u : 7u;
    const bool due = !valid || hot > 0.5 || (((uint)slot + (uint)frame) & period) == 0;
    float4 ray = 0;
    float3 dir = 0;
    float hit = 0;
    if (due)
    {
        dir = ProbeRayDir(gi, frame + c * 0.5);
        ray = RayTrace(ProbePos(wc, c) + offset, dir, voxParams.z, frac(gi * 0.618034 + frame * 0.381966), hit);
    }
    // The distance kept up to 16 m (the classification below asks whether the ray met anything at all); the
    // visibility's moments clamp it at ProbeMaxDistance. Backface hits by the cells' L1 direction were tried and
    // dropped: a thin roof lit from outside has its L1 pointing out as the ground has (probes under a shed's roof counted
    // as inside and were moved through it into the sun: blotches on ceilings), and a test of thickness missed the
    // probes under the beach (the ground's fill is thinner than three cells).
    g_probeRay[gi] = ray;
    g_probeDir[gi] = float4(dir, min(hit, 16));
    if (gi == 0)
        g_probeHot = 0;
    GroupMemoryBarrierWithGroupSync();
    const float4 old = historyTex.Load(int3(texel, 0));
    const int2 dt = ProbeDepthTile(slot, c) + int2(tid.xy); // the distances' texel, laid out as the light's
    const float2 oldM = srcTex.Load(int3(dt, 0)).xy;
    if (!due)
    {
        outTex[texel] = old;
        probeDepthOut[dt] = float4(oldM, 0, 1);
        if (gi == 0)
        {
            outTex[st] = state;
            outTex[ot] = float4(offset, 0);
        }
        return;
    }
    // The texel's interior one (the border copies the octahedral neighbour across the edge) and its direction: light by
    // cosine weights, distances by cos^8.
    const float3 n = OctDecode((OctInterior(int2(tid.xy), 6) + 0.5) / 6 * 2 - 1);
    float4 sum = 0;
    float weights = 0;
    float2 m = 0;
    float mw = 0;
    float sL = 0, sL2 = 0, sW2 = 0; // the rays' luminance: weighted sum, of squares, the weights' squares
    const float maxDistance = ProbeMaxDistance(sp);
    [loop] for (int r = 0; r < 64; r++)
    {
        const float w = max(dot(n, g_probeDir[r].xyz), 0);
        sum += g_probeRay[r] * w;
        weights += w;
        const float lr = dot(g_probeRay[r].rgb, float3(0.2126, 0.7152, 0.0722));
        sL += lr * w;
        sL2 += lr * lr * w;
        sW2 += w * w;
        const float c2 = w * w, c4 = c2 * c2, wd = c4 * c4; // cos^8 (kProbeDepthSharpness)
        const float dist = min(g_probeDir[r].w, maxDistance);
        m += float2(dist, dist * dist) * wd;
        mw += wd;
    }
    const float4 fresh = sum / max(weights, 1e-4);
    const float2 freshM = mw > 1e-4 ? m / mw : oldM;
    probeDepthOut[dt] = float4(valid ? lerp(freshM, oldM, 0.95) : freshM, 0, 1);
    // Hysteresis 0.95 (64 rays a fourth frame: ~1300 rays' worth; at 0.8 the probes boiled and one frame's look at the
    // stair varied by 2.5 of 255 between runs). A faster blend on big changes (0.6 when the light doubled or halved) made
    // dim probes jump at their updates: the noise of 64 rays crossed that line (the user: voxels light up and go out).
    // A change beyond the rays' noise (4 sigma of this texel's weighted mean, and 10% of its light) is taken faster (0.7)
    // and keeps the probe updating every frame for 12 frames: a flashlight, a lamp switched on, a fire (the user,
    // 2026-11-15: the shotgun's flashlight did not show with the probes). The noise alone keeps 0.95 (the fixed faster
    // blend on big changes before made dim probes jump).
    const float meanL = sL / max(weights, 1e-4), oldL = dot(old.rgb, float3(0.2126, 0.7152, 0.0722));
    const float noise = sqrt(max(sL2 / max(weights, 1e-4) - meanL * meanL, 0) * sW2) / max(weights, 1e-4);
    const bool changed = fast && valid && abs(meanL - oldL) > 4 * noise + 0.1 * max(meanL, oldL) + 0.002;
    if (changed)
        InterlockedOr(g_probeHot, 1u);
    outTex[texel] = valid ? lerp(fresh, old, changed ? 0.7 : 0.95) : fresh;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        // Inside geometry: most rays end within 0.35 m (in a wall); or under the ground: the rays going up nearly all
        // meet the ground's underside within 16 m while those going down nearly all meet nothing (the grid is empty below
        // the ground's fill). Under a roof the floor stops the rays going down.
        float closeShare = 0, frontMin = 1e9, upRays = 0, upClosed = 0, upMin = 1e9, downRays = 0, downOpen = 0;
        float3 frontDir = 0;
        [loop] for (int k = 0; k < 64; k++)
        {
            const float d = g_probeDir[k].w, dz = g_probeDir[k].z;
            closeShare += d < 0.35 * sp ? 1 : 0;
            if (d < frontMin)
            {
                frontMin = d;
                frontDir = g_probeDir[k].xyz;
            }
            if (dz > 0.3)
            {
                upRays++;
                if (d < 15.9)
                {
                    upClosed++;
                    upMin = min(upMin, d);
                }
            }
            if (dz < -0.3)
            {
                downRays++;
                downOpen += d >= 15.9 ? 1 : 0;
            }
        }
        const bool underground = upClosed > 0.8 * upRays && downOpen > 0.8 * downRays && upRays > 0 && downRays > 0;
        // Kept slowly (0.9): a probe by a wall whose share of 64 rays wavered went on and off.
        const float inside = underground ? 1 : saturate(closeShare / 48);
        outTex[st] = float4(ProbeKey(wc), 1 + (valid ? lerp(inside, state.w - 1, 0.9) : inside));
        // Relocation (DDGI's, by our classification): under the ground up through it (to the surface if it is within
        // reach), else away from a surface nearer than 0.3 of the spacing; within 0.45 of the spacing of the centre.
        float3 moved = offset;
        if (underground)
            moved.z += upMin + 0.25 * sp;
        else if (frontMin < 0.3 * sp)
            moved -= frontDir * (0.3 * sp - frontMin);
        outTex[ot] = float4(clamp(moved, -0.45 * sp, 0.45 * sp), g_probeHot != 0 ? 12 : max(hot - 1, 0));
    }
}

// GI v2: the light (rgb) and occlusion (a) at P for the normal N from the 8 probes around it, with DDGI's weights
// (trilinear, the side the normal faces, Chebyshev visibility from the probe's distances, probes inside walls nearly
// out). The point is moved off the surface towards the normal and the camera (DDGI's bias: a surface's own distance
// otherwise fails the visibility). inside: how far P lies in the probe grid (0 at its edge, 1 from two probes in), for
// the blend with the cones.
float4 ProbeLight(float3 P, float3 N, int c, out float inside)
{
    const float sp = CascadeSpacing(c);
    const int3 origin = ProbeOrigin(c);
    const float3 V = normalize(float3(toWorldX.w, toWorldY.w, toWorldZ.w) - P);
    const float3 Pb = P + (N * 0.2 + V * 0.8) * (0.3 * sp);
    const float3 g = Pb / sp - 0.5 - origin;
    const float3 room = min(g, (kProbeN - 1) - g);
    // The outer cascade hands over to the cones over its last 6 probes (2 before 2026-11-14: walking at mine the probes'
    // edge was a step, a wall x0.89 -> x0.69 against no GI from 15 to 22 m); the near one to the outer over 2.
    inside = saturate(min(room.x, min(room.y, room.z)) / (c == kProbeCascades - 1 ? 6 : 2));
    if (inside <= 0)
        return 0;
    const int3 base = (int3)floor(g);
    const float3 f = g - base;
    const float2 oct = OctEncode(N) * 0.5 + 0.5;
    float4 sum = 0;
    float weights = 0;
    [unroll] for (int i = 0; i < 8; i++)
    {
        const int3 off = int3(i & 1, (i >> 1) & 1, i >> 2);
        const int3 wc = origin + base + off;
        const int slot = ProbeSlot(wc);
        const float4 state = probeTex.Load(int3(ProbeStateTexel(slot, c), 0));
        if (state.w < 1 || any(state.xyz != ProbeKey(wc)))
            continue;
        const float3 tri = lerp(1 - f, f, (float3)off);
        const float3 probePos = ProbePos(wc, c) + probeTex.Load(int3(ProbeOffsetTexel(slot, c), 0)).xyz; // relocated
        const float facing = (dot(normalize(probePos - P), N) + 1) * 0.5;
        float w = (facing * facing + 0.2) * lerp(1, 0.02, saturate((state.w - 1.4) / 0.4));
        // Visibility: the biased point's distance from the probe against the probe's distances that way.
        const float3 fromProbe = Pb - probePos;
        const float dist = length(fromProbe);
        const float2 dOct = OctEncode(fromProbe / max(dist, 1e-4)) * 0.5 + 0.5;
        const float2 m = probeDepthTex.SampleLevel(linearClamp, (ProbeDepthTile(slot, c) + 1 + dOct * 6) / kProbeDepthAtlas, 0).xy;
        const float variance = abs(m.y - m.x * m.x);
        const float over = max(dist - m.x, 0);
        const float chebyshev = dist <= m.x ? 1 : variance / (variance + over * over);
        if (visAreas[0].w > 0) // negative: no visibility test (CRYMT_GI_PROBEVIS=0, to measure what it stops)
            w *= max(chebyshev * chebyshev * chebyshev, 0.05);
        // Tiny weights crushed (DDGI): a sliver of a probe's weight no longer lifts a dark corner.
        if (w < 0.2)
            w *= w * w / 0.04;
        w *= tri.x * tri.y * tri.z;
        sum += probeTex.SampleLevel(linearClamp, (ProbeTile(slot, c) + 1 + oct * 6) / kProbeAtlas, 0) * w;
        weights += w;
    }
    if (weights <= 0)
    {
        inside = 0;
        return 0;
    }
    return sum / weights;
}

[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)giSize.x || id.y >= (uint)giSize.y)
        return;
    float2 uv = GiDepthUv(id.xy);
    float4 result = 0;
    bool thin = false;
    float d0 = depthTex.SampleLevel(pointClamp, uv, 0);
    if (d0 >= kWeaponDepth && d0 < 0.99999)
    {
        float3 P = ViewPos(uv);
        float2 px = GiScale() / fullSize; // one GI texel: smoother normals than neighbouring depth pixels
        float3 pr = ViewPos(uv + float2(px.x, 0)), pl = ViewPos(uv - float2(px.x, 0));
        float3 pd = ViewPos(uv + float2(0, px.y)), pu = ViewPos(uv - float2(0, px.y));
        // A thin feature in front (a grass blade, twig, leaf edge a texel or two wide): the neighbours on both sides along
        // x or y lie farther by a step. Its normal from depth is arbitrary (both differences cross to what is behind),
        // so its occlusion was chance: the cones lifted along it started inside the bush (black) or outside it (full
        // ambient), bright strokes on black shade under canopy that moved with the jitter and the wind.
        float jump = 0.05 * P.z + 0.05;
        thin = ((uint)options & 256) != 0 && ((pr.z - P.z > jump && pl.z - P.z > jump) || (pd.z - P.z > jump && pu.z - P.z > jump));
        float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
        float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
        float3 N = normalize(cross(dx, dy));
        // A wide estimate (central differences three GI texels each way) averages across the faces of low-poly round
        // meshes, where the narrow one is flat per face (faces showed in the GI). Used where each pair's midpoint lies on
        // the surface (within 15% of the pair's span: no crease or step in the window); options & 8: narrow only.
        if (((uint)options & 8) == 0)
        {
            float2 wide = px * 3;
            float3 wr = ViewPos(uv + float2(wide.x, 0)), wl = ViewPos(uv - float2(wide.x, 0));
            float3 wd = ViewPos(uv + float2(0, wide.y)), wu = ViewPos(uv - float2(0, wide.y));
            float3 sx = wr - wl, sy = wd - wu;
            bool smooth = length((wr + wl) * 0.5 - P) < 0.15 * length(sx) && length((wd + wu) * 0.5 - P) < 0.15 * length(sy);
            float3 Nw = cross(sx, sy);
            if (smooth && dot(Nw, Nw) > 1e-12)
                N = normalize(Nw);
        }
        if (dot(N, P) > 0)
            N = -N; // facing the camera
        float noise = frac(52.9829189 * frac(dot(float2(id.xy), float2(0.06711056, 0.00583715))) + ((uint)options & 1 ? 0 : frame * 0.618034));
        // (The screen part - bounce and occlusion traced on screen with a visibility bitmask, CRYMT_GI_SCREEN, off
        // since 2026-11-10 - was removed 2026-11-15: noisy, at odds with the rest of the GI; tag experiments-2026-11-15.)
        // Near the GI alone, then over 1.5 x that the engine's baked occlusion takes over (the shader patch's
        // CryMT_GIBakedMix, the same window): full up to options >> 16 (5 bits) x 2 metres, none from 2.5 x that on.
        float fadeStart = (float)(((uint)options >> 16) & 31u) * 2;
        float fade = fadeStart > 0 ? saturate((fadeStart * 2.5 - P.z) / (fadeStart * 1.5)) : saturate(1 - P.z / maxDistance);
        if (voxParams.y > 0)
        {
            // Five cones of 60 degrees (the normal and four around it, turned per texel) through the voxel grid, from a
            // point lifted off the surface by two cells, from two cells on: the lowest edge of a side cone's first
            // sample stays about a cell above the surface (no self-occlusion); near geometry off screen counts too.
            float3 Pe = float3(P.xy, -P.z), Ne = float3(N.xy, -N.z);
            float3 Pw = float3(dot(float4(Pe, 1), toWorldX), dot(float4(Pe, 1), toWorldY), dot(float4(Pe, 1), toWorldZ));
            float3 Nw = normalize(float3(dot(Ne, toWorldX.xyz), dot(Ne, toWorldY.xyz), dot(Ne, toWorldZ.xyz)));
            float3 T = normalize(cross(abs(Nw.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0), Nw));
            float3 B = cross(Nw, T);
            float3 O = Pw + Nw * (voxParams.x * 2);
            float start = voxParams.x * 2;
            const int area = VisAreaAt(Pw);
            if (area >= 0 && visAreas[0].y > 0.5)
                SetRoom(area);
            // GI v2: the probes inside their grid, the cones beyond it and across its last two probes. |visAreas[0].w|:
            // the probes' strength, + 100 when the cones keep the occlusion everywhere and the probes give the light only
            // (the user, 2026-11-14: walking, surfaces the GI had darkened came out lighter where the probes took over:
            // a probe hangs up to a metre off the surface and misses the occlusion close to it).
            float probeIn = 0;
            const float probeW = abs(visAreas[0].w);
            const bool probeLightOnly = probeW >= 100;
            const float probeStrength = probeLightOnly ? probeW - 100 : probeW;
            // The finest cascade that holds the point, blended to the next over its last two probes (0.5 -> 1 -> 2 m).
            float4 probe = 0;
            if (visAreas[0].w != 0)
            {
                float rest = 1; // the share the finer cascades left
                [unroll] for (int pc = 0; pc < kProbeCascades; pc++)
                    if (rest > 0)
                    {
                        float inC = 0;
                        const float4 pl = ProbeLight(Pw, Nw, pc, inC);
                        probe += pl * inC * rest;
                        probeIn += inC * rest;
                        rest *= 1 - inC;
                    }
                probe = probeIn > 0 ? probe / probeIn : 0;
            }
            float3 farLight = 0;
            float farOcc = 0;
            if (probeIn < 1 || probeLightOnly)
            {
                float4 cone = ConeTrace(O, Nw, 0.577, start, voxParams.z, false, true, Pw, Nw, noise);
                farLight = cone.rgb;
                farOcc = cone.a;
                [unroll] for (int c = 0; c < 4; c++)
                {
                    float a = (c + noise) * 1.5707963;
                    float3 dir = normalize(Nw * 0.5 + (T * cos(a) + B * sin(a)) * 0.866);
                    cone = ConeTrace(O, dir, 0.577, start, voxParams.z, false, true, Pw, Nw, noise + c * 0.25);
                    farLight += cone.rgb * 0.5;
                    farOcc += cone.a * 0.5;
                }
                farLight *= voxParams.w / 3;
                farOcc /= 3;
            }
            if (visAreas[0].w != 0 && probeIn < 1)
            {
                // The cones' light matched to the probes': at the grid's edge (both traced) every 8th pixel each way adds
                // the two lights' luminance to this frame's sums; the probes pass turns last frame's into the ratio.
                const float3 kLum = float3(0.2126, 0.7152, 0.0722);
                if (probeIn > 0 && ((id.x | id.y) & 7) == 0)
                {
                    const uint now = ((uint)frame & 1) * 3;
                    uint ignored;
                    InterlockedAdd(probeMatch[now], (uint)(dot(probe.rgb * probeStrength, kLum) * 4096), ignored);
                    InterlockedAdd(probeMatch[now + 1], (uint)(dot(farLight, kLum) * 4096), ignored);
                    InterlockedAdd(probeMatch[now + 2], 1, ignored);
                }
                if (probeMatch[7] != 0)
                    farLight *= asfloat(probeMatch[6]);
            }
            farLight = lerp(farLight, probe.rgb * probeStrength, probeIn); // the probes' own strength (CRYMT_GI_PROBESTRENGTH)
            if (!probeLightOnly)
                farOcc = lerp(farOcc, probe.a, probeIn);
            // Indoors: a VisArea; else the cover above (houses without one): the roof texture half a metre off the
            // surface and, when the two strengths differ, a ray up leaned off the surface and turned per texel. The
            // VisArea is asked half a metre in front of the surface: its 0.3 m margin took a building's outer walls for
            // its room (mine's yard: an outer wall kept the indoor strength, x0.16 against no GI).
            float roof = VisAreaAt(Pw + Nw * 0.5) >= 0 ? 1 : 0;
            if (roof < 1 && ((uint)options & 128) != 0) // CRYMT_GI_ROOFCELLS=1
                roof = max(roof, RoofAt(Pw + Nw * (voxParams.x * 2)));
            if (roof < 1 && occRange.y != motionParams.w)
            {
                float ra = noise * 6.2831853;
                float3 up = normalize(float3(0, 0, 1) + Nw * 0.5 + (T * cos(ra) + B * sin(ra)) * 0.25);
                const float cover = RoofCover(O, up, frac(noise + 0.5));
                if (occRange.y != motionParams.w)
                {
                // A roof alone is no room: an eave over an open yard, a canopy (the user, mine 2026-11-14: a wall under
                // an eave took the indoor strength and the GI darkened it six times). Four level rays (turned per texel)
                // tell how closed in the point is; a quarter (the point's own wall) counts as open.
                float closed = 0;
                [unroll] for (int k = 0; k < 4; k++)
                {
                    const float a = ra + k * 1.5707963;
                    closed += RoofCover(O, float3(cos(a), sin(a), 0), frac(noise + 0.25 * k));
                }
                roof = max(roof, cover * saturate((closed / 4 - 0.25) / 0.5));
                }
            }
            if (debug == 4) // the indoor share (VisArea, roof) as occlusion
            {
                outTex[id.xy] = float4(0, 0, 0, roof * 0.8);
                return;
            }
            // Near occlusion (Num3 / CRYMT_GI_NEAROCC with the voxelized geometry): five short cones (2 m) from close to
            // the surface (voxParams2.y/z cells), occlusion only, each sample counting only for its part above the
            // surface's own plane (else the surface shades itself). Adds darkening under and beside nearby objects;
            // the cones above keep the light and the look of foliage.
            if (voxParams2.w > 0)
            {
                float3 On = Pw + Nw * (voxParams.x * voxParams2.y);
                float nearStart = voxParams.x * voxParams2.z;
                float near = ConeTrace(On, Nw, 0.577, nearStart, 2.0, true, true, Pw, Nw, noise).a;
                [unroll] for (int k = 0; k < 4; k++)
                {
                    float a = (k + noise + 0.5) * 1.5707963;
                    float3 dir = normalize(Nw * 0.5 + (T * cos(a) + B * sin(a)) * 0.866);
                    near += ConeTrace(On, dir, 0.577, nearStart, 2.0, true, true, Pw, Nw, noise + k * 0.25).a * 0.5;
                }
                farOcc = 1 - (1 - farOcc) * (1 - saturate(near / 3 * voxParams2.w));
            }
            // The grid's outer edge: the voxel part fades over the outer level's last 16 cells (32 m), else it ended in a
            // sharp line where the pixels left the grid (the user, village beach 2026-11-13: a hard edge that did not move
            // with the GI's distance).
            const float outer = saturate(EdgeCells(3, Pw, 0) / 16);
            farLight *= outer;
            farOcc *= outer;
            result = float4(farLight, Strengthen(saturate(farOcc * voxParams2.x), roof)) * fade;
        }
        // No grid (not ready yet, CRYMT_GI_VOXELS=0): no GI.
    }
    if (debug == 2) // CRYMT_GI_DEBUG=2: the occlusion only, 3: the bounce light only (measurements), 4: the VisArea mask
        result.rgb = 0;

    if (debug == 3)
        result.a = 0;
    // Thin: marked for the filter with the occlusion stored as -1 - a (a in 0..1); the filter gives it its surroundings'.
    outTex[id.xy] = thin ? float4(result.rgb, -1 - result.a) : result;
}

// 5x5 depth-aware blur of the raw result, then the blend with the previous GI where this point was seen last frame
// (reprojected with the camera motion; no history where the previous distance there differs: disocclusion).
[numthreads(8, 8, 1)]
void filter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)giSize.x || id.y >= (uint)giSize.y)
        return;
    float2 uv = (id.xy + 0.5) / giSize;
    if (depthTex.SampleLevel(pointClamp, GiDepthUv(id.xy), 0) < kWeaponDepth)
    {
        outTex[id.xy] = 0;
        distOut[id.xy] = 0;
        return;
    }
    float zc = Distance(GiDepthUv(id.xy));
    float4 sum = 0, sum2 = 0, around = 0;
    float weights = 0, aroundWeights = 0;
    float4 own = srcTex.SampleLevel(pointClamp, uv, 0);
    bool thinHere = own.a < -0.5;
    [unroll] for (int y = -2; y <= 2; y++)
        [unroll] for (int x = -2; x <= 2; x++)
        {
            float2 suv = uv + float2(x, y) / giSize;
            float spatial = 3 - 0.5 * (abs(x) + abs(y));
            float w = exp(-abs(Distance(GiDepthUv(int2(id.xy) + int2(x, y))) - zc) / (0.03 * zc + 0.05)) * spatial;
            float4 s = srcTex.SampleLevel(pointClamp, suv, 0);
            if (s.a < -0.5)
            {
                // A thin feature's own value counts only for itself, when nothing around is a surface.
                if (x == 0 && y == 0)
                {
                    s.a = -1 - s.a;
                    sum += s * w * 0.01;
                    sum2 += s * s * w * 0.01;
                    weights += w * 0.01;
                }
                continue;
            }
            sum += s * w;
            sum2 += s * s * w;
            weights += w;
            around += s * spatial;
            aroundWeights += spatial;
        }
    if (thinHere && aroundWeights > 0)
    {
        // The surfaces around it, whatever their depth (it sits on or in front of them: their cover is its cover).
        float4 a = around / aroundWeights;
        sum = a;
        sum2 = a * a;
        weights = 1;
    }
    float3 V = ViewPos(GiDepthUv(id.xy));
    float4 prevClip = mul(float4(V.xy, -V.z, 1), reproj);
    float2 prevUv = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5;
    float history = 0;
    if (prevClip.w > 0 && all(prevUv >= 0) && all(prevUv <= 1))
    {
        // Seen last frame if one of the 2x2 texels the history is filtered from was at this distance (one texel
        // alone misses on distant slopes, where the distance changes fast from row to row).
        int2 p0 = int2(floor(prevUv * giSize - 0.5));
        float tolerance = 0.04 * prevClip.w + 0.05;
        [unroll] for (int j = 0; j < 4; j++)
            if (abs(distPrev[clamp(p0 + int2(j & 1, j >> 1), 0, int2(giSize) - 1)] - prevClip.w) < tolerance)
                history = temporal;
    }
    // History kept within the spread of this frame's neighbourhood: what it no longer has (moved shadows, revealed
    // surfaces) does not trail.
    float4 current = sum / max(weights, 1e-4);
    float4 sigma = sqrt(max(sum2 / max(weights, 1e-4) - current * current, 0));
    float4 result = current;
    float4 previous = historyTex.SampleLevel(linearClamp, prevUv, 0);
    if (history > 0 && Finite(previous))
        result = lerp(current, clamp(previous, current - 1.5 * sigma - 0.01, current + 1.5 * sigma + 0.01), history);
    outTex[id.xy] = Finite(result) ? result : 0;
    if (debug == 1 && history == 0 && V.z < maxDistance) // CRYMT_GI_DEBUG=1: magenta where there was no history
        outTex[id.xy] = float4(4, 0, 4, 1);
    distOut[id.xy] = V.z;
}

// Full-size GI for the engine's pixel shaders: each pixel blends the GI texels around it (3x3, a tent over their depth
// samples, which sit on every second pixel) weighted by how close their distance is to its own, so fine geometry (a
// character's straps, leaves, gravel) no longer takes the GI of the surface behind it; with no texel at its distance it
// takes the closest one. Weapon and sky pixels get none. t0 depth, t1 filtered GI, u0 out, u1 the GI texels' distances.
[numthreads(8, 8, 1)]
void upsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float d = depthTex.Load(int3(id.xy, 0));
    // The shader patch's flags: a + 2 everywhere the GI is in effect, sky included (its pixels use no ambient), so
    // filtering between texels never mixes them; which of the engine's terms it replaces in the top-left texel, a / 2
    // (bits: 1 SSAO, 2 the objects' RAM, 4 their sky amount, 8 TerrainAO near the camera, 16 the terrain's vertex AO,
    // 32 blended particles) plus 64 x half the metres up to which the GI replaces TerrainAO, rgb 0.
    uint mask = (((uint)options & 512) ? 1u : 0u) | (((uint)options >> 10) & 30u);
    float flags = mask ? 2.0 : 0.0;
    if (id.x == 0 && id.y == 0)
    {
        // 32: the particles' GI (options 2^21); 64 x half TerrainAO's distance (options >> 16, 5 bits): a / 2 < 2048.
        uint control = mask | ((((uint)options >> 21) & 1u) << 5) | ((((uint)options >> 16) & 31u) << 6);
        outTex[id.xy] = float4(0, 0, 0, 2.0 * control);
        return;
    }
    if (d < kWeaponDepth)
    {
        // The first-person view: the camera's GI with the flags, or nothing (the engine's own terms) without it.
        outTex[id.xy] = ((uint)options & 16) ? historyTex.Load(int3(0, 0, 0)) + float4(0, 0, 0, flags) : 0;
        return;
    }
    if (d >= 0.99999)
    {
        outTex[id.xy] = float4(0, 0, 0, flags);
        return;
    }
    float z = depthParams.y / (d + depthParams.x);
    float tolerance = 0.03 * z + 0.05;
    int f = GiScale();
    int2 g0 = int2(id.xy) / f;
    int2 r = int2(id.xy) - g0 * f;
    float4 sum = 0;
    float weights = 0, nearest = 1e30;
    float4 nearestValue = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            int2 g = clamp(g0 + int2(x, y), 0, int2(giSize) - 1);
            float2 delta = float2(r - int2(x, y) * f);
            float spatial = max(0, 1 - abs(delta.x) / (1.25 * f)) * max(0, 1 - abs(delta.y) / (1.25 * f));
            if (spatial <= 0)
                continue;
            float gz = distOut[g];
            float4 v = srcTex.Load(int3(g, 0));
            float dz = abs(gz - z);
            float w = spatial * exp(-dz / tolerance);
            sum += v * w;
            weights += w;
            if (dz < nearest)
            {
                nearest = dz;
                nearestValue = v;
            }
        }
    outTex[id.xy] = (weights > 1e-3 ? sum / weights : nearestValue) + float4(0, 0, 0, flags);
}

// GI for the first-person view (weapon, hands, objects held: drawn with their own camera into depth under kWeaponDepth,
// where the screen part has nothing to go by): one value for the frame, 32 cones from the eye over the sphere through
// the voxel grid (mean light times the cones' strength, mean occlusion times theirs), blended over frames in u0 (1x1:
// the previous value in, the new out). The upsample gives it to those pixels (options & 16).
groupshared float4 cameraCones[32];
[numthreads(32, 1, 1)]
void camera(uint3 id : SV_DispatchThreadID)
{
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float k = id.x + 0.5;
    float z = 1 - 2 * k / 32;
    float r = sqrt(saturate(1 - z * z));
    float phi = k * 2.39996323 + frac(frame * 0.618034) * 6.2831853;
    float3 dir = float3(r * cos(phi), r * sin(phi), z);
    cameraCones[id.x] = ConeTrace(eye, dir, 0.577, voxParams.x * 2, voxParams.z, false, false, eye, dir, frac(frame * 0.618034 + k * 0.1));
    GroupMemoryBarrierWithGroupSync();
    if (id.x != 0)
        return;
    float4 sum = 0;
    for (int i = 0; i < 32; i++)
        sum += cameraCones[i];
    sum /= 32;
    float roof = max(VisInside(eye), ((uint)options & 128) != 0 ? RoofAt(eye) : 0); // blended over frames below
    float4 value = float4(sum.rgb * voxParams.w, Strengthen(sum.a * voxParams2.x, roof));
    float4 previous = outTex[uint2(0, 0)];
    outTex[uint2(0, 0)] = temporal > 0 && Finite(previous) ? lerp(value, previous, 0.9) : value;
}

// Motion vectors for DLSS (full size, screen units, previous minus current, camera motion included): t0 depth, t1 object
// motion, t2 bent vegetation, u0 out. Every point moves with the camera; objects the engine's motion blur pass drew
// (moving ones, and the first-person weapon and hands) by their own before, which has the camera's move in it (the
// pass's MotionBlurData is the previous view x the previous world matrix; the first-person view's are relative to the
// camera, so there it is the sway, bob and recoil). The weapon has no camera motion of its own (0 where not drawn).
// t1: motionParams.y 2 CryMT's target from the patched pass (xy before minus now, z distance / far, w 1 exact, 0.5 with
// the engine's shutter factor f still in, 0 not drawn); 1 a copy of the engine's own (rg (now - before) x f x (1 -
// distance / 200)^8, zeroed under 0.001; b distance / far; a its id, non-zero where drawn). radius: the frame's f.
[numthreads(8, 8, 1)]
void motion(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float2 uv = (id.xy + 0.5) / fullSize;
    float d = depthTex[id.xy];
    bool weapon = d < kWeaponDepth;
    float2 mv = 0;
    float3 V = 0;
    if (!weapon)
    {
        V = ViewPosAt(uv, d);
        float4 prevClip = mul(float4(V.xy, -V.z, 1), reproj);
        mv = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5 - uv;
    }
    // Taken only where the object is this pixel's (its distance matches the scene's; the weapon's is in its own range)
    // and the move stays under a quarter of the screen a frame.
    bool object = false;
    if (motionParams.y > 1.5)
    {
        float4 o = srcTex[id.xy];
        bool exact = o.w > 0.75;
        float2 obj = exact ? o.xy : o.xy / max(radius, 1e-3);
        float dist = o.z * motionParams.x;
        if ((exact || (o.w > 0.25 && radius > 1e-3)) && (weapon || abs(dist - V.z) < 0.03 * V.z + 0.05) && all(abs(obj) < 0.25) && all(obj == obj))
        {
            mv = obj;
            object = true;
        }
    }
    else if (motionParams.y > 0.5 && radius > 1e-3)
    {
        // History: until 2026-11-13 this read the target after the engine had reused it for a copy of the scene
        // (KeepObjectMotion copies it now right after its pass), and subtracted it from the camera's move as if it were the
        // object's own at f = 1 (it has the camera's move in it, times f: 0.72 at 60 fps).
        float4 v = srcTex[id.xy];
        float dist = v.z * motionParams.x;
        float fade = pow(1 - saturate(dist / 200), 8);
        float2 obj = fade > 0.002 ? -v.xy / (fade * radius) : 0;
        if (v.a != 0 && any(v.xy != 0) && fade > 0.002 && (weapon || abs(dist - V.z) < 0.03 * V.z + 0.05) && all(abs(obj) < 0.25) && all(obj == obj))
        {
            mv = obj;
            object = true;
        }
    }
    // Wind-bent vegetation (t2, the patched z-prepass: before minus now per 1/60 s, motionParams.z the frame's scale).
    if (!weapon && !object && motionParams.w > 0)
    {
        float2 bent = historyTex[id.xy].xy * motionParams.z;
        if (all(abs(bent) < 0.1) && all(bent == bent))
            mv += bent;
    }
    outTex[id.xy] = float4(mv, 0, 0);
}

// The scene colour for DLSS without negative, NaN or infinite components (Device::RunSanitize, CRYMT_DLSS_CLAMP=1):
// t1 the colour, u0 the copy DLSS reads. The engine's tone mapping clips such values to black; DLSS may not.
[numthreads(8, 8, 1)]
void sanitize(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float4 c = srcTex[id.xy];
    c.rgb = (c.rgb >= 0 && c.rgb < 65504) ? c.rgb : 0; // NaN fails both
    outTex[id.xy] = c;
}

// The same in place on u0 (CRYMT_DLSS_CLAMP=out: DLSS's output, before the engine's tone mapping reads it).
[numthreads(8, 8, 1)]
void sanitizeOut(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float4 c = outTex[id.xy];
    c.rgb = (c.rgb >= 0 && c.rgb < 65504) ? c.rgb : 0;
    outTex[id.xy] = c;
}

// GTAO in its own passes (Device::RunGtao; CRYMT_GI_GTAO strength, CRYMT_GI_GTAORADIUS metres; with the engine's SSAO
// replaced). At 5120x1440 the GI runs at a quarter of the size: the occlusion computed there was coarse and shimmered
// in the upsampling. Like the engine's SSAO: half size, a fixed 4x4 pattern of slice turns (no change from frame to
// frame), a depth-aware 4x4 blur that averages the pattern away, then a factor on the full-size GI.
// gtao: t0 depth, t1 the foliage mask (options 2^22: its pixels occlude nothing), t2 the sun's shadow mask (red;
// options 2^23: foliage in the shadow gets 0.2 of the occlusion: there the ambient is all its light and the occlusion
// showed too strong), u1 the occlusion. Jimenez et al. 2016 / XeGTAO: per slice the normal projected, horizons
// with a radius falloff, quadratic steps from a pixel out, the cosine-weighted visible arcs.
static const float kBayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float ZAt(int2 px)
{
    return depthParams.y / (depthTex.Load(int3(px, 0)) + depthParams.x);
}

// GTAO's size: full (options & 32768) or half. Its own history: options & 1024 (reprojected, 0.9).
int AoScale()
{
    return ((uint)options & 32768) ? 1 : 2;
}

bool AoHistory()
{
    return ((uint)options & 1024) != 0;
}

[numthreads(8, 8, 1)]
void gtao(uint3 id : SV_DispatchThreadID)
{
    int aoS = AoScale();
    int2 aoSize = int2((fullSize + aoS - 1) / aoS);
    if (any(int2(id.xy) >= aoSize))
        return;
    int2 px = min(int2(id.xy) * aoS, int2(fullSize) - 1);
    float2 uv = (px + 0.5) / fullSize;
    float d0 = depthTex.Load(int3(px, 0));
    if (d0 < kWeaponDepth || d0 >= 0.99999)
    {
        distOut[id.xy] = 0;
        return;
    }
    float3 P = ViewPosAt(uv, d0);
    // The normal from the full-size depth a pixel apart (the smaller difference each way).
    int2 lim = int2(fullSize) - 1;
    float3 pr = ViewPosAt((min(px + int2(1, 0), lim) + 0.5) / fullSize, depthTex.Load(int3(min(px + int2(1, 0), lim), 0)));
    float3 pl = ViewPosAt((max(px - int2(1, 0), 0) + 0.5) / fullSize, depthTex.Load(int3(max(px - int2(1, 0), 0), 0)));
    float3 pd = ViewPosAt((min(px + int2(0, 1), lim) + 0.5) / fullSize, depthTex.Load(int3(min(px + int2(0, 1), lim), 0)));
    float3 pu = ViewPosAt((max(px - int2(0, 1), 0) + 0.5) / fullSize, depthTex.Load(int3(max(px - int2(0, 1), 0), 0)));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0)
        N = -N;
    float gR = motionParams.x > 0 ? motionParams.x : radius;
    float2 gRUv = min(gR / (P.z * invProj) * 0.5, 0.15);
    float radiusPx = length(gRUv * fullSize);
    if (radiusPx < 2)
    {
        distOut[id.xy] = 0;
        return;
    }
    // A fixed pattern (the 4x4 blur averages it away), the same every frame: turns that moved per frame for DLAA to
    // accumulate made a still view shimmer (DLAA saw the picture change while nothing moved).
    float rot = (kBayer4[(id.x & 3) + (id.y & 3) * 4] + 0.5) / 16;
    float stepNoise = frac(rot * 7 + 0.37);
    float3 Vv = normalize(-P);
    float minS = 1.0 / radiusPx;
    float falloffMul = -1 / (gR * 0.615), falloffAdd = 0.385 / 0.615 + 1;
    const int slices = 2, gsteps = 6;
    float vis = 0;
    [unroll] for (int k = 0; k < slices; k++)
    {
        float a = (k + rot) * (3.14159265 / slices);
        float2 dir = float2(cos(a), sin(a));
        float3 dvec = ViewPosAt(uv + dir / fullSize, d0) - P; // the slice's direction at this depth
        float3 ortho = dvec - dot(dvec, Vv) * Vv;
        float3 axis = normalize(cross(ortho, Vv));
        float3 pn = N - axis * dot(N, axis);
        float pnLen = length(pn);
        float cosN = saturate(dot(pn, Vv) / max(pnLen, 1e-5));
        float n = (dot(ortho, pn) < 0 ? -1.0 : 1.0) * acos(cosN);
        float low0 = cos(n + 1.5707963), low1 = cos(n - 1.5707963);
        float hc0 = low0, hc1 = low1; // side 0: along dir, side 1: against
        [loop] for (int st = 0; st < gsteps; st++)
        {
            float sN = (st + stepNoise) / gsteps;
            sN = sN * sN + minS;
            float2 offset = dir * gRUv * sN;
            [unroll] for (int side = 0; side < 2; side++)
            {
                float2 suv = uv + (side == 0 ? offset : -offset);
                if (any(suv < 0) || any(suv > 1))
                    continue;
                int2 spx = min(int2(suv * fullSize), lim);
                float sd = depthTex.Load(int3(spx, 0));
                if (sd < kWeaponDepth || (((uint)options & 4194304) && srcTex.Load(int3(spx, 0)).x > 0.5))
                    continue;
                float3 dv = ViewPosAt((spx + 0.5) / fullSize, sd) - P;
                float dist = length(dv);
                float wgt = saturate(dist * falloffMul + falloffAdd);
                float shc = lerp(side == 0 ? low0 : low1, dot(dv / max(dist, 1e-5), Vv), wgt);
                if (side == 0)
                    hc0 = max(hc0, shc);
                else
                    hc1 = max(hc1, shc);
            }
        }
        float h0 = -acos(clamp(hc1, -1, 1)), h1 = acos(clamp(hc0, -1, 1));
        h0 = n + clamp(h0 - n, -1.5707963, 1.5707963);
        h1 = n + clamp(h1 - n, -1.5707963, 1.5707963);
        float sinN = sin(n);
        vis += pnLen * ((cosN + 2 * h0 * sinN - cos(2 * h0 - n)) + (cosN + 2 * h1 * sinN - cos(2 * h1 - n))) * 0.25;
    }
    float occlusion = saturate(1 - vis / slices);
    if (((uint)options & 8388608) && srcTex.Load(int3(px, 0)).x > 0.5)
        occlusion *= lerp(1, 0.2, saturate(historyTex.Load(int3(px, 0)).x));
    distOut[id.xy] = occlusion;
}

// The 4x4 blur (the pattern's size), depth-aware, then blended with the previous frame's (t2, reprojected with the
// camera; kept only where the distance it stored matches, and within the spread of this frame's samples), 0.9: about
// ten frames. t0 depth, t1 the raw occlusion, u0 the history out (x occlusion, y distance). temporal 0: a camera cut.
[numthreads(8, 8, 1)]
void gtaoBlur(uint3 id : SV_DispatchThreadID)
{
    int aoS = AoScale();
    int2 aoSize = int2((fullSize + aoS - 1) / aoS);
    if (any(int2(id.xy) >= aoSize))
        return;
    int2 lim = int2(fullSize) - 1;
    int2 pc = min(int2(id.xy) * aoS, lim);
    float dc = depthTex.Load(int3(pc, 0));
    if (dc < kWeaponDepth || dc >= 0.99999)
    {
        outTex[id.xy] = float4(0, 0, 0, 0);
        return;
    }
    float zc = ZAt(pc);
    float sum = 0, sum2 = 0, weights = 0;
    [unroll] for (int y = -2; y <= 1; y++)
        [unroll] for (int x = -2; x <= 1; x++)
        {
            int2 t = clamp(int2(id.xy) + int2(x, y), 0, aoSize - 1);
            int2 p = min(t * aoS, lim);
            float d = depthTex.Load(int3(p, 0));
            if (d < kWeaponDepth || d >= 0.99999)
                continue;
            float w = exp(-abs(ZAt(p) - zc) / (0.02 * zc + 0.05));
            float v = srcTex.Load(int3(t, 0)).x;
            sum += v * w;
            sum2 += v * v * w;
            weights += w;
        }
    float current = weights > 0 ? sum / weights : 0;
    float sigma = weights > 0 ? sqrt(max(sum2 / weights - current * current, 0)) : 0;
    float result = current;
    if (temporal > 0 && AoHistory())
    {
        float3 V = ViewPosAt((pc + 0.5) / fullSize, dc);
        float4 prevClip = mul(float4(V.xy, -V.z, 1), reproj);
        float2 prevUv = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5;
        if (prevClip.w > 0 && all(prevUv >= 0) && all(prevUv <= 1))
        {
            float2 prev = historyTex.SampleLevel(pointClamp, prevUv, 0).xy;
            // the weight: visAreas[kProbeOptions].w (CRYMT_GI_GTAOHISTORY / the AO page's "Накопление GTAO"; was 0.9)
            const float w = visAreas[kProbeOptions].w > 0 ? visAreas[kProbeOptions].w : 0.9;
            if (abs(prev.y - prevClip.w) < 0.04 * prevClip.w + 0.05)
                result = lerp(current, clamp(prev.x, current - 1.5 * sigma - 0.03, current + 1.5 * sigma + 0.03), w);
        }
    }
    outTex[id.xy] = float4(result, zc, 0, 0);
}

// On the full-size GI (u0, read and written): the occlusion of the half-size texels around the pixel (bilinear times
// depth likeness), times the strength, as the engine's SSAO was: rgb (1 - ao), share 1 - (1 - share)(1 - ao), the
// flags (a = share + 2 * flags) kept. t0 depth, t1 the accumulated occlusion (x).
[numthreads(8, 8, 1)]
void gtaoApply(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float d = depthTex.Load(int3(id.xy, 0));
    if (d < kWeaponDepth || d >= 0.99999 || (id.x == 0 && id.y == 0)) // the top-left texel holds the shaders' control
        return;
    float z = depthParams.y / (d + depthParams.x);
    int2 aoSize = int2((fullSize + 1) / 2);
    int2 lim = int2(fullSize) - 1;
    float2 tc = (id.xy + 0.5) * 0.5 - 0.5;
    int2 t0 = int2(floor(tc));
    float2 f = tc - t0;
    float sum = 0, weights = 0;
    [unroll] for (int j = 0; j < 4; j++)
    {
        int2 o = int2(j & 1, j >> 1);
        int2 t = clamp(t0 + o, 0, aoSize - 1);
        float bil = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
        float w = bil * (exp(-abs(ZAt(min(t * 2, lim)) - z) / (0.02 * z + 0.05)) + 1e-3);
        sum += srcTex.Load(int3(t, 0)).x * w;
        weights += w;
    }
    float ao = saturate((AoScale() == 1 ? srcTex.Load(int3(id.xy, 0)).x : sum / max(weights, 1e-5)) * motionParams.y);
    float4 g = outTex[id.xy];
    float flags = floor(g.a * 0.5);
    float share = g.a - 2 * flags;
    // Leaves (t2, the foliage mask 1): the GI's occlusion and bounce times CRYMT_GI_FOLIAGEGI, the share kept under 0.9
    // (the ambient floor). Grass (the mask 0.6): times CRYMT_GI_GRASSGI and no GTAO - the engine's SSAO stays on it (its
    // blend with the terrain's colour takes only that; with GTAO in its place and the leaves' 1.5 it came out flat and too
    // bright, the user). motionParams.z for this pass: the leaves' factor + 4 x floor(the grass's x 100).
    // + 4096 x floor(the leaves' bounce factor x 100): the leaves' bounce light apart from their occlusion
    // (CRYMT_GI_FOLIAGEBOUNCE; the user, 2026-11-14: bounce +50% overall, -50% on the leaves).
    float mask = historyTex.Load(int3(id.xy, 0)).x;
    if (mask > 0.5)
    {
        float b100 = floor(motionParams.z / 4096);
        float rest = motionParams.z - b100 * 4096;
        float packed = floor(rest / 4);
        float k = mask < 0.8 ? packed / 100 : rest - packed * 4;
        if (mask < 0.8)
            ao = 0;
        g.rgb *= mask < 0.8 ? k : b100 / 100;
        share = min(share * k, max(share, 0.9));
    }
    outTex[id.xy] = float4(g.rgb * (1 - ao), 1 - (1 - share) * (1 - ao) + 2 * flags);
}

// The sun's visibility from a point (the particles' volumes): a ray towards it through the finest level that holds each
// step, from half a metre off, steps of a cell of that level or 8% of the distance, up to 48 m; 1 open .. 0 covered.
float SunVisible(float3 O, float3 dir)
{
    float covered = 0, d = 0.5;
    [loop] for (int i = 0; i < 64 && covered < 0.95 && d < 48; i++)
    {
        float3 X = O + dir * d;
        int level = 0;
        [loop] while (level < 3 && !InGrid(level, X, 0))
            level++;
        if (!InGrid(level, X, 0))
            break;
        covered += (1 - covered) * SampleGrid(level, X, 0).a;
        d += max(voxParams.x * (1 << level), d * 0.08);
    }
    return 1 - covered;
}

// The particles' occlusion volumes (Device::RunGi, for the patched Particles.cfi): reproj[0] = the coarse cell size (m),
// cells x, y, z; reproj[1] = slices across, the fine cell size; reproj[2] = towards the sun, w 1 when there is one. Two
// volumes side by side in a 2D atlas (u0, R32G32F; fine left, coarse right), each the cells around the camera (the first
// floor(eye / cell) - dims / 2) as z-slices tiled across: per cell x the share of ambient a blended particle there loses
// - what a surface facing up would take from the voxels: five cones up (60 degrees), the indoor share, Strengthen - and
// y the sun's visibility (SunVisible; -1 without a sun), z the indoor share; below the shares' row (one.y) the tints
// (rows one.y + 1 on: the hue of the cones' light, part white); reproj[3] = the outdoor part of the share, its cap, the
// tint's part. Row one.y: texels 0-3 the fine volume's first cell
// relative to the camera (metres) and cell size, 4-7 the coarse one's. One volume a frame, by turns (each keeps its place
// with its cells, so particles read it right a frame later; 0.79 ms a frame for both, the sun's rays most of it).
[numthreads(8, 8, 1)]
void pvolume(uint3 id : SV_DispatchThreadID)
{
    int3 dims = int3(reproj[0].yzw);
    int tx = (int)reproj[1].x;
    int2 one = int2(dims.x * tx, dims.y * ((dims.z + tx - 1) / tx));
    if (id.x >= (uint)one.x || id.y >= (uint)one.y)
        return;
    int cascade = (int)reproj[1].z; // one volume a frame, by turns (reproj[1].z: 0 fine, 1 coarse)
    int2 lid = int2(id.xy);
    id.x += cascade * one.x;
    float cell = cascade == 0 ? reproj[1].y : reproj[0].x;
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float3 first = floor(eye / cell) - float3(dims / 2);
    if (lid.y == 0 && lid.x < 4)
    {
        float4 place = float4(first * cell - eye, cell);
        outTex[int2(cascade * 4 + lid.x, one.y)] = place[lid.x];
    }
    int2 tile = lid / dims.xy;
    int z = tile.x + tile.y * tx;
    if (z >= dims.z)
    {
        outTex[id.xy] = 0;
        outTex[int2(id.x, id.y + one.y + 1)] = 1;
        return;
    }
    float3 P = (first + float3(lid - tile * dims.xy, z) + 0.5) * cell;
    float3 up = float3(0, 0, 1), T = float3(1, 0, 0), B = float3(0, 1, 0);
    float start = voxParams.x * 2;
    float4 cone = ConeTrace(P, up, 0.577, start, voxParams.z, false, false, P, up, 0.5);
    float occ = cone.a;
    float3 light = cone.rgb;
    [unroll] for (int c = 0; c < 4; c++)
    {
        float a = (c + 0.5) * 1.5707963;
        float3 dir = normalize(up * 0.5 + (T * cos(a) + B * sin(a)) * 0.866);
        cone = ConeTrace(P, dir, 0.577, start, voxParams.z, false, false, P, up, 0.5 + c * 0.25);
        occ += cone.a * 0.5;
        light += cone.rgb * 0.5;
    }
    // The tint: the hue of the light the cones bring (luminance 1), reproj[3].z of it, the rest white; white where they
    // bring almost none (night, outside the grid).
    float lum = dot(light, float3(0.2126, 0.7152, 0.0722));
    float3 hue = lum > 1e-3 ? clamp(light / lum, 0, 3) : 1;
    outTex[int2(id.x, id.y + one.y + 1)] = float4(lerp(1, hue, reproj[3].z), 0);
    float roof = VisInside(P);
    if (roof < 1 && occRange.y != motionParams.w)
        roof = max(roof, RoofCover(P, up, 0.5));
    // Outdoors a part of the occlusion (reproj[3].x; light haze and dust vanished against the background), at most
    // reproj[3].y; z the indoor share (the particles take the voxels' sun indoors, the game's outdoors); w the share
    // before the outdoor part and the cap (the GI in the fog applies its own: RunVfog).
    float raw = Strengthen(saturate(occ / 3 * voxParams2.x), roof);
    float share = min(raw * lerp(reproj[3].x, 1, roof), reproj[3].y);
    float sun = reproj[2].w > 0.5 ? SunVisible(P, normalize(reproj[2].xyz)) : -1;
    outTex[id.xy] = float4(share, sun, roof, raw);
}

// Exposure for DLSS (Device::RunExposure, one group): the average log2 luminance of the scene's HDR colour (t1) over a
// 64x32 grid, exposure = radius (the key) / that, clamped to [occlusion, maxDistance] and eased toward per frame by
// bounce (temporal 1: at once); u1 the 1x1 R32F exposure, read and written. DLSS's own auto exposure left night shade
// almost black to it: its reconstruction then turned the noise there into white single-pixel sparkles.
groupshared float gsLum[256];
[numthreads(16, 16, 1)]
void exposure(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    float sum = 0;
    [unroll] for (int k = 0; k < 8; k++)
    {
        float2 uv = (float2(id.x + (k & 3) * 16, id.y + (k >> 2) * 16) + 0.5) / float2(64, 32);
        float lum = dot(srcTex.SampleLevel(pointClamp, uv, 0).rgb, float3(0.2126, 0.7152, 0.0722));
        lum = lum >= 0 ? min(lum, 1e4) : 0; // NaN, negative
        sum += log2(max(lum, 1e-5));
    }
    gsLum[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint n = 128; n > 0; n >>= 1)
    {
        if (gi < n)
            gsLum[gi] += gsLum[gi + n];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        float target = clamp(radius / exp2(gsLum[0] / 2048), occlusion, maxDistance);
        float previous = distOut[uint2(0, 0)];
        distOut[uint2(0, 0)] = previous > 0 && temporal < 1 ? exp2(lerp(log2(previous), log2(target), bounce)) : target;
    }
}
)CRYMT"
