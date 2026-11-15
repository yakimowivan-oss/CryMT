// CryMT: the voxel grids: injection, light, mips, the debug view.
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kVoxShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
cbuffer VoxConstants : register(b0)
{
    row_major float4x4 worldToView;     // the engine's view space (x, y, -distance)
    row_major float4x4 worldToPrevClip; // the previous frame's clip space (where its colour is)
    float4 toWorldX; float4 toWorldY; float4 toWorldZ; // view (x, y, z, 1) dotted with these: world x, y, z
    float2 fullSize; float2 invProj;
    float2 depthParams; float voxelSize; float mode;
    float4 gridMin;     // xyz the grid's first cell, w cells per axis
    float4 prevGridMin; // xyz the last update's
};
Texture2D<float4> depthTex : register(t0);  // update: scene depth (r); view: the info panel
Texture2D<float4> colourTex : register(t1);
Texture2D<float4> sceneTex : register(t2);
Texture2D<float> foliageTex : register(t2); // update: the z-prepass's foliage mask (1 on alpha-tested surfaces), null without
RWTexture3D<float4> voxels : register(u0);
RWTexture2D<float4> viewOut : register(u1);
RWTexture3D<uint> occupancy : register(u1); // update: the voxelized geometry's frame stamps (levels 0-2)
RWTexture3D<uint> seen : register(u3);      // update, light: bit 0 the screen gave the cell its colour; bits 1-7
                                            // how many updates in a row it showed the cell empty (kClearAfter)
// Kept geometry goes when the screen showed its cell empty this many updates in a row with no new stamp: one depth sample
// a cell and update flips between a thin part, the DLAA jitter and LOD changes, and the cells came and went.
static const uint kClearAfter = 4; // updates: 4, 8, 16 frames at levels 0-2 (periods 1, 2, 4)
RWTexture3D<unorm float4> albedo : register(u4); // light: the material colour the geometry pass stored (a 1 when set)
RWTexture3D<float4> voxelsDst : register(u2);
RWTexture3D<float4> voxels1 : register(u3); // view: grid levels 1..3 (their first cells in worldToPrevClip rows 1..3)
RWTexture3D<float4> voxels2 : register(u4);
RWTexture3D<float4> voxels3 : register(u5);
cbuffer VoxLamps : register(b1) // light: the lamps (DlssSceneInfo::lamps), world metres
{
    float4 lampPos[8];    // xyz, radius
    float4 lampColour[8]; // rgb (HDR, times CRYMT_GI_LAMPS), the cosine of a projector's half angle (-2: a point light)
    float4 lampDir[8];    // a projector's direction
    float4 lampCount;
};
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);
RWTexture3D<unorm float4> dirSrc : register(u3); // downsample: the per-axis opacity, the mip below (grid mip 2 and up)
RWTexture3D<unorm float4> dirDst : register(u4); // and the one written
RWTexture3D<unorm float> roofOut : register(u5);  // roof: cover above per cell of level 0 at half its resolution
// The cells' light direction (CRYMT_GI_DIRLIGHT 1, voxFlags bit 0): g, |g| <= 1; the light going out towards w is the
// colour times 1 + g.w (a thin wall lit outside: the colour the mean of both sides, g pointing out). Downsample: u6 the
// mip below, u7 the mip written.
RWTexture3D<snorm float4> l1Grid : register(u6);
RWTexture3D<snorm float4> l1Dst : register(u7);
cbuffer VoxFlags : register(b2) { uint voxFlags; };
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
// Six directions (CRYMT_GI_DIRLIGHT 2, voxFlags bit 1): the light going out towards +X, -X, +Y, -Y, +Z, -Z (kSixAxis).
// u8..u13 the level (downsample: the mip below), u14..u19 the downsample's mip.
RWTexture3D<float3> six0 : register(u8);
RWTexture3D<float3> six1 : register(u9);
RWTexture3D<float3> six2 : register(u10);
RWTexture3D<float3> six3 : register(u11);
RWTexture3D<float3> six4 : register(u12);
RWTexture3D<float3> six5 : register(u13);
RWTexture3D<float3> sixDst0 : register(u14);
RWTexture3D<float3> sixDst1 : register(u15);
RWTexture3D<float3> sixDst2 : register(u16);
RWTexture3D<float3> sixDst3 : register(u17);
RWTexture3D<float3> sixDst4 : register(u18);
RWTexture3D<float3> sixDst5 : register(u19);
static const float3 kSixAxis[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
float3 SixLoad(int f, uint3 p)
{
    [forcecase] switch (f)
    {
    case 0: return six0[p];
    case 1: return six1[p];
    case 2: return six2[p];
    case 3: return six3[p];
    case 4: return six4[p];
    default: return six5[p];
    }
}
void SixStore(int f, uint3 p, float3 c)
{
    [forcecase] switch (f)
    {
    case 0: six0[p] = c; break;
    case 1: six1[p] = c; break;
    case 2: six2[p] = c; break;
    case 3: six3[p] = c; break;
    case 4: six4[p] = c; break;
    default: six5[p] = c; break;
    }
}
void SixDstStore(int f, uint3 p, float3 c)
{
    [forcecase] switch (f)
    {
    case 0: sixDst0[p] = c; break;
    case 1: sixDst1[p] = c; break;
    case 2: sixDst2[p] = c; break;
    case 3: sixDst3[p] = c; break;
    case 4: sixDst4[p] = c; break;
    default: sixDst5[p] = c; break;
    }
}

static const float kWeaponDepth = 0.1;
static const float kColourTrust = 48; // metres: screen colour taken at full weight up to here (update)
bool Finite3(float3 v) { return all((asuint(v) & 0x7f800000) != 0x7f800000); }
int3 Wrap(int3 cell) { int n = (int)gridMin.w; return (cell % n + n) % n; }

// The visible surface at a pixel in the view space of worldToView (x, y, -distance); far plane: very far.
float3 SurfaceAt(float2 uv)
{
    float d = depthTex.SampleLevel(pointClamp, uv, 0).r;
    float z = d >= 0.99999 ? 1e9 : depthParams.y / (d + depthParams.x);
    return float3(float2(uv.x * 2 - 1, 1 - uv.y * 2) * invProj * z, -z);
}

// Of two differences to the neighbours, the one across less depth (not over an edge).
float3 Nearer(float3 a, float3 b) { return abs(a.z) < abs(b.z) ? a : b; }
float3 Tone(float3 c) { return c / (1 + c); }

// The previous frame's colour at puv (a cell's reprojection) belongs to the first-person view (weapon, hands: depth under
// kWeaponDepth) where the screen shows it now, the bilinear footprint and a pixel and a half around: it stays in place
// while the world turns behind it, so cells coming out from behind it took the hands' colour into the GI's bounce (the
// user, 2026-11-13: "the hands shade other objects in the background").
bool WeaponAt(float2 puv)
{
    float2 px = 1.5 / fullSize;
    float d = depthTex.SampleLevel(pointClamp, puv, 0).r;
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(-px.x, -px.y), 0).r);
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(px.x, -px.y), 0).r);
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(-px.x, px.y), 0).r);
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(px.x, px.y), 0).r);
    return d < kWeaponDepth;
}

// CRYMT_GI_DIRLIGHT 1: the screen's colour c is the light the cell sends out towards the camera's side of its surface
// (nView, view space): that side moves to it, the other side keeps its light (a cell seen for the first time: dark,
// unknown); the colour becomes the two sides' mean and g their difference along the normal over their sum (the rest of g
// fades). A wall seen from inside keeps the sun its outer side got.
void ScreenL1(uint3 id, inout float4 v, float3 nView, float3 c, float trust)
{
    float3 n = normalize(float3(dot(nView, toWorldX.xyz), dot(nView, toWorldY.xyz), dot(nView, toWorldZ.xyz)));
    float3 g = l1Grid[id].xyz;
    float gn = dot(g, n);
    bool fresh = v.a <= 0.01;
    float3 front = fresh ? c * trust : lerp(v.rgb * (1 + gn), c, 0.25 * trust);
    float3 back = fresh ? 0 : v.rgb * max(1 - gn, 0);
    v.rgb = 0.5 * (front + back);
    float lf = Luma(front), lb = Luma(back);
    float3 gNew = n * ((lf - lb) / max(lf + lb, 1e-4)) + (g - n * gn) * 0.75;
    float len = length(gNew);
    l1Grid[id] = float4(len > 1 ? gNew / len : gNew, 0);
}

// CRYMT_GI_DIRLIGHT 2: the screen's colour goes into the faces turned to the camera's side of the surface (by how much
// they face it), the others keep theirs (a cell seen for the first time: dark).
void ScreenSix(uint3 id, float3 nView, float3 c, float trust, bool fresh)
{
    float3 n = normalize(float3(dot(nView, toWorldX.xyz), dot(nView, toWorldY.xyz), dot(nView, toWorldZ.xyz)));
    [unroll] for (int f = 0; f < 6; f++)
    {
        float w = dot(n, kSixAxis[f]);
        if (fresh)
            SixStore(f, id, w > 0.2 ? c * trust : 0);
        else if (w > 0)
            SixStore(f, id, lerp(SixLoad(f, id), c, 0.25 * trust * saturate(w * 2)));
    }
}

[numthreads(4, 4, 4)]
void update(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w;
    if (any(id >= (uint)n))
        return;
    int3 g = (int3)gridMin.xyz, pg = (int3)prevGridMin.xyz;
    int3 cell = g + Wrap(int3(id) - g); // the cell of the current grid stored here
    float4 v = voxels[id];
    bool entered = any(cell < pg) || any(cell >= pg + n);
    if (entered)
    {
        v = 0; // entered the grid now: nothing known
        if (voxFlags & 1)
            l1Grid[id] = 0;
        if (voxFlags & 2)
            [unroll] for (int f0 = 0; f0 < 6; f0++)
                SixStore(f0, id, 0);
    }
    bool track = prevGridMin.w > 0; // the geometry's levels keep which cells the screen coloured
    if (track && entered)
    {
        seen[id] = 0;
        // The geometry stamps stored here belong to the cell that left on the far side (toroidal addressing): kept only
        // if this frame's voxelization (stamp: mode) already wrote the new cell, else from the next frame on they stood
        // as a wall of cells at the grid's edge for the stamps' lifetime, each time the grid moved (the user: a wall of
        // voxels at the edge of their reach, now and then; a cube like a sky dome in the Num2 view).
        if ((occupancy[id] >> 8) != (uint)abs(mode))
            occupancy[id] = 0;
    }
    // The z-prepass draws voxelized into this level (RunVoxelGeometry): the stamp of the last frame that drew into the
    // cell; a recent one makes it solid, also where the screen never showed the surface (undersides, backs).
    // Kept (mode < 0, CRYMT_GI_GEOMPERSIST): any stamp, until the screen shows the cell empty or it leaves the grid;
    // else the last three voxelizations only (geometry the engine stopped drawing, a room seen through a portal once
    // the camera left it, went from the grid: the GI jumped when entering).
    uint geometry = prevGridMin.w > 0 && all(cell >= pg) && all(cell < pg + n) ? occupancy[id] : 0;
    // A stamp at all (without voxel geometry prevGridMin.w is 0 and every empty cell passed the age test: all were taken
    // for foliage and eased to opacity 0 - the user: Num3 off left no occlusion at all).
    bool stamped = geometry != 0 && ((geometry >> 8) >= (uint)prevGridMin.w || (mode < 0 && (geometry >> 8) > 0));
    float stampA = (geometry & 255) / 255.0; // solid 1, foliage CRYMT_GI_FOLIAGE (drawn without its alpha test)
    // Foliage cells (a partly open stamp: the voxelized leaves, by their shape) take their opacity from the stamps alone,
    // eased: the screen made them solid where it saw a leaf and halved them where it saw through a gap between leaves, so
    // they changed every update as the leaves swayed and the camera moved, and the GI under trees flickered (the user:
    // only turning the GI off stopped it). The screen still gives them their colour.
    bool foliage = stamped && stampA < 0.98;
    // CRYMT_GI_OCCUPANCY=geometry (voxels page Num0): the opacity from the stamps alone, the screen gives the colour; what
    // the screen saw is not made solid or open (it depends on where the camera is: cells came and went as it moved).
    bool geomOnly = track && frac(abs(mode)) > 0.25;
    float3 centre = (cell + 0.5) * voxelSize;
    float3 V = mul(float4(centre, 1), worldToView).xyz;
    float dist = -V.z;
    // The engine cube's stamps (RunVoxelGeometry: opacity byte 253, frame 1, so never newer than a z-prepass stamp) fill
    // what the camera does not see; in its view the z-prepass and the screen decide (the cube may draw another level of
    // detail: those cells would come back at every turn of the cube and go again).
    if (stamped && (geometry & 255) == 253 && dist > 0.3 && all(abs(V.xy / (dist * invProj)) <= 1))
        stamped = false;
    if (dist > 0.3)
    {
        float2 ndc = V.xy / (dist * invProj);
        float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (all(uv >= 0) && all(uv <= 1))
        {
            float d = depthTex.SampleLevel(pointClamp, uv, 0).r;
            if (d >= kWeaponDepth)
            {
                float surface = d >= 0.99999 ? 1e9 : depthParams.y / (d + depthParams.x);
                float tolerance = voxelSize * 0.87;
                // Where the cell is against the surface: along the ray it lay far in front of a surface seen at a
                // grazing angle (a cell on a road a third of a cell above the asphalt, seen at 10 degrees: nearly two
                // cells), and was carved out: stripes of holes across roads by the phase of their steps, dark spots,
                // cells that came and went as the camera moved (the user). Near the surface it is measured along the
                // surface's normal (from the depth of the neighbouring pixels); far in front along the ray as before.
                float side = dist - surface; // < 0 in front of the surface, > 0 behind it
                float3 surfN = 0;            // the surface's normal there (view space, towards the camera), if found
                if (d < 0.99999 && side > -(tolerance + 24 * voxelSize) && side < tolerance + 3 * voxelSize)
                {
                    float2 px = 1 / fullSize;
                    float3 P0 = SurfaceAt(uv);
                    float3 tx = Nearer(SurfaceAt(uv + float2(px.x, 0)) - P0, P0 - SurfaceAt(uv - float2(px.x, 0)));
                    float3 ty = Nearer(SurfaceAt(uv + float2(0, px.y)) - P0, P0 - SurfaceAt(uv - float2(0, px.y)));
                    float3 N = cross(tx, ty);
                    if (dot(N, N) > 1e-12)
                    {
                        N = normalize(N);
                        if (dot(N, P0) > 0)
                            N = -N; // towards the camera
                        side = -dot(V - P0, N);
                        surfN = N;
                    }
                }
                if (side < -tolerance)
                {
                    // Seen through: halved, unless the voxelized geometry says something is there (thin things - a
                    // fence, a pole, branches - the screen sees past: halved here, set again by the next voxelization,
                    // they came and went as the camera moved). Stamps of things that went are cleared below.
                    if (!foliage && !stamped && !geomOnly)
                        v.a *= 0.5;
                    if (track)
                    {
                        // Kept geometry (mode < 0) that the screen shows empty goes (a moved object, a door) after
                        // kClearAfter such updates in a row; a recent stamp (drawn within the window) restarts the
                        // count. Not foliage: the screen sees through the gaps between leaves from most places, and the
                        // kept leaves went and came back as the camera moved (the crowns flickered while walking).
                        const bool fresh = (occupancy[id] >> 8) >= (uint)prevGridMin.w;
                        const uint empty = fresh ? 0 : min((seen[id] >> 1) + 1, 127u);
                        seen[id] = empty << 1;
                        if (mode < 0 && !foliage && !fresh && empty >= kClearAfter)
                            occupancy[id] = 0;
                    }
                }
                else if (side < tolerance)
                {
                    float4 pc = mul(float4(centre, 1), worldToPrevClip);
                    float2 puv = float2(pc.x, -pc.y) / pc.w * 0.5 + 0.5;
                    float3 c = colourTex.SampleLevel(linearClamp, puv, 0).rgb;
                    if (pc.w > 0 && all(puv >= 0) && all(puv <= 1) && Finite3(c) && !WeaponAt(puv))
                    {
                        c = clamp(c, 0, 8);
                        // Colour seen from afar is mostly fog and haze (at night lighter than the surfaces: the
                        // coarse levels lit a band at their edge until the camera came near): trusted fully up to
                        // kColourTrust metres, less beyond; a first sight from afar is dimmed likewise.
                        float trust = saturate(kColourTrust / dist);
                        if ((voxFlags & 2) && dot(surfN, surfN) > 0)
                            ScreenSix(id, surfN, c, trust, v.a <= 0.01);
                        if ((voxFlags & 1) && dot(surfN, surfN) > 0)
                            ScreenL1(id, v, surfN, c, trust);
                        else
                            v.rgb = v.a > 0.01 ? lerp(v.rgb, c, 0.25 * trust) : c * trust;
                        // A leaf on screen (the foliage mask) is no wall: half open at most, as the stamps have it.
                        if (!foliage && !geomOnly)
                            v.a = lerp(v.a, foliageTex.SampleLevel(pointClamp, uv, 0) >= 0.5 ? 0.5 : 1, 0.5);
                        if (track)
                            seen[id] = 1;
                    }
                }
                else if (side < tolerance + 2 * voxelSize && !foliage && !geomOnly && foliageTex.SampleLevel(pointClamp, uv, 0) < 0.5)
                {
                    // Just behind a solid visible surface: the inside of the ground, a wall, a rock, filled with the
                    // surface's colour. The screen gave a surface one layer of cells, in steps along a slope with
                    // nothing between them (the user: holes across a road in the Num2 view, dark spots that went with
                    // the camera); behind foliage stays as it was (leaves are thin).
                    float4 pc = mul(float4(centre, 1), worldToPrevClip);
                    float2 puv = float2(pc.x, -pc.y) / pc.w * 0.5 + 0.5;
                    float3 c = colourTex.SampleLevel(linearClamp, puv, 0).rgb;
                    if (pc.w > 0 && all(puv >= 0) && all(puv <= 1) && Finite3(c) && !WeaponAt(puv))
                    {
                        float trust = saturate(kColourTrust / dist);
                        v.rgb = v.a > 0.01 ? lerp(v.rgb, clamp(c, 0, 8), 0.25 * trust) : clamp(c, 0, 8) * trust;
                        v.a = lerp(v.a, 1, 0.5);
                        if (track)
                            seen[id] = 1;
                    }
                }
            }
        }
    }
    if (geomOnly)
        v.a = stamped ? (foliage ? lerp(v.a, stampA, 0.3) : stampA) : v.a * 0.5; // no stamp: gone within a few updates
    else if (stamped)
        v.a = foliage ? lerp(v.a, stampA, 0.3) : max(v.a, stampA);
    voxels[id] = v;
}

// The lamps' light at a cell (light pass): falloff (1 - d^2 / r^2)^2, a projector's cone with a soft edge, on the side
// the neighbours say faces the lamp, where a march through this level towards it (stopping a cell and a half short of
// it: the fixture) finds no cover.
float3 LampLight(int3 cell, float3 N, bool flat, int3 g, int n)
{
    float3 sum = 0;
    float3 P = (cell + 0.5) * voxelSize;
    [loop] for (int k = 0; k < (int)lampCount.x; k++)
    {
        float3 D = lampPos[k].xyz - P;
        float r = lampPos[k].w, dd = dot(D, D);
        if (dd >= r * r)
            continue;
        float d = sqrt(dd);
        float3 Ld = D / max(d, 1e-3);
        float att = saturate(1 - dd / (r * r));
        float cosHalf = lampColour[k].w;
        float cone = cosHalf > -1 ? saturate((dot(-Ld, lampDir[k].xyz) - cosHalf) / max(1 - cosHalf, 1e-3) * 4) : 1;
        float face = flat ? 0.5 : saturate(dot(N, Ld));
        float lit = att * att * cone * face;
        if (lit <= 0)
            continue;
        float cells = d / voxelSize, step = max(1, (cells - 3) / 16), cover = 0;
        float3 q = cell + 0.5 + Ld * 1.5;
        [loop] for (float t = 1.5; t < cells - 1.5 && cover < 0.95; t += step)
        {
            int3 c = (int3)floor(q);
            if (any(c < g) || any(c >= g + n))
                break;
            cover += (1 - cover) * voxels[Wrap(c)].a;
            q += Ld * step;
        }
        sum += lampColour[k].rgb * lit * (1 - cover);
    }
    return sum;
}

// Light for cells the voxelized geometry made solid but the screen never coloured (seen 0), at every update of the level
// (the time of day moves): the sun's colour (row 1 of worldToPrevClip, which this pass does not need) times an albedo
// (row 2 x), on the side the opacity of the six neighbours says faces the sun (row 0 xyz, towards it), where a march
// through this level towards it finds no cover; plus a sky fill (row 2 y, a share of the sun's colour, more facing up)
// where a march straight up finds open sky (none under a roof); plus the lamps (b1). Row 2 z: also cells the screen
// coloured (CRYMT_GI_LAMPTEST). A quarter of the way to it per update: lamps culled, broken or moving fade instead of
// switching the cells' light from one update to the next.
[numthreads(4, 4, 4)]
void light(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w;
    if (any(id >= (uint)n))
        return;
    float4 v = voxels[id];
    if (v.a < 0.5 || ((seen[id] & 1) != 0 && worldToPrevClip[2].z == 0))
        return;
    float3 L = worldToPrevClip[0].xyz;
    int3 g = (int3)gridMin.xyz;
    int3 cell = g + Wrap(int3(id) - g);
    float3 grad = float3(voxels[Wrap(cell - int3(1, 0, 0))].a - voxels[Wrap(cell + int3(1, 0, 0))].a,
                         voxels[Wrap(cell - int3(0, 1, 0))].a - voxels[Wrap(cell + int3(0, 1, 0))].a,
                         voxels[Wrap(cell - int3(0, 0, 1))].a - voxels[Wrap(cell + int3(0, 0, 1))].a);
    bool flat = dot(grad, grad) <= 0.01;
    float3 N = flat ? 0 : normalize(grad);
    float facing = flat ? 0.5 : saturate(dot(N, L));
    // The material's colour where the geometry pass sampled one (scaled so an average texture of 0.5 gives the albedo
    // setting), else the setting itself (grey).
    float4 material = albedo[id];
    float3 surface = worldToPrevClip[2].x * (material.a > 0 ? material.rgb / 0.5 : 1);
    float3 albedoSun = worldToPrevClip[1].rgb * surface;
    float3 sky = albedoSun * worldToPrevClip[2].y * (flat ? 0.75 : 0.5 + 0.5 * N.z);
    float cover = 0;
    float3 p = cell + 0.5 + L * 1.5;
    const int sunSteps = worldToPrevClip[2].w > 0 ? (int)worldToPrevClip[2].w : 24; // CRYMT_GI_SUNMARCH, cells of this level
    [loop] for (int i = 0; i < sunSteps && cover < 0.95 && facing > 0; i++)
    {
        int3 c = (int3)floor(p);
        if (any(c < g) || any(c >= g + n))
            break;
        cover += (1 - cover) * voxels[Wrap(c)].a;
        p += L;
    }
    float roof = 0;
    p = cell + 0.5 + float3(0, 0, 1.5);
    [loop] for (int j = 0; j < 24 && roof < 0.95; j++)
    {
        int3 c = (int3)floor(p);
        if (any(c < g) || any(c >= g + n))
            break;
        roof += (1 - roof) * voxels[Wrap(c)].a;
        p.z += 1;
    }
    const float3 sunPart = albedoSun * facing * (1 - cover);
    const float3 rest = sky * (1 - roof) + surface * LampLight(cell, N, flat, g, n);
    if (voxFlags & 1)
    {
        // CRYMT_GI_DIRLIGHT 1: the sun on the side towards it only (a thin wall: its outer face), the sky and the lamps on
        // both: the cell sends rest + sun out towards the sun's side and rest away from it.
        const float3 target = rest + 0.5 * sunPart;
        const float lt = Luma(target);
        const float3 side = flat ? L : N * (dot(N, L) >= 0 ? 1 : -1);
        const float3 dirT = lt > 1e-5 ? side * (0.5 * Luma(sunPart) / lt) : 0;
        v.rgb = lerp(v.rgb, target, 0.25);
        l1Grid[id] = float4(lerp(l1Grid[id].xyz, dirT, 0.25), 0);
        if (voxFlags & 2) // six directions: the sun into the faces turned to the lit side, the rest into all
            [unroll] for (int f = 0; f < 6; f++)
                SixStore(f, id, lerp(SixLoad(f, id), rest + sunPart * saturate(2 * dot(side, kSixAxis[f])), 0.25));
    }
    else
        v.rgb = lerp(v.rgb, sunPart + rest, 0.25);
    voxels[id] = v;
}

// The engine cube's face just drawn (CRYMT_GI_CUBEINJECT, RunCubeInject): every cell of this level that the face saw as
// a surface, outside the camera's view, gets the cube's stamp (frame 1, opacity byte 253: the update counts it only
// outside the view; never newer than a geometry stamp) and the face's lit colour, and is marked coloured (seen bit 0:
// the light pass leaves it). worldToPrevClip: the face's view-projection (clip.w = the distance along its axis);
// toWorldX: its depth (distance = y / (d + x)) and size; t0 its depth, t1 its HDR colour; prevGridMin: the level's cells.
[numthreads(4, 4, 4)]
void inject(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w;
    if (any(id >= (uint)n))
        return;
    int3 g = (int3)prevGridMin.xyz;
    int3 cell = g + Wrap(int3(id) - g);
    float3 centre = (cell + 0.5) * voxelSize;
    float4 clip = mul(float4(centre, 1), worldToPrevClip);
    if (clip.w < 1.5) // the face's near plane (1.5 m with entities)
        return;
    float2 ndc = clip.xy / clip.w;
    if (any(abs(ndc) > 1))
        return;
    // In the camera's view the z-prepass and the screen decide.
    float3 V = mul(float4(centre, 1), worldToView).xyz;
    if (-V.z > 0.3 && all(abs(V.xy / (-V.z * invProj)) <= 1))
        return;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float tolerance = voxelSize * 0.87;
    // The face sees well past the cell (every texel of the 3x3 around it beyond it): a cube stamp there is stale (an
    // entity that moved, a door) and goes; the geometry's stamps stay (the update and the screen decide on those).
    float nearest = 1e9;
    [unroll] for (int j = -1; j <= 1; j++)
        [unroll] for (int i = -1; i <= 1; i++)
        {
            float dn = depthTex.SampleLevel(pointClamp, uv + float2(i, j) / toWorldX.zw, 0).r;
            nearest = min(nearest, dn >= 0.99999 ? 1e9 : toWorldX.y / (dn + toWorldX.x));
        }
    if (clip.w < nearest - 2 * tolerance)
    {
        if (occupancy[id] == 256u + 253u)
        {
            occupancy[id] = 0;
            float4 gone = voxels[id];
            gone.a = 0;
            voxels[id] = gone;
        }
        return;
    }
    float d = depthTex.SampleLevel(pointClamp, uv, 0).r;
    if (d >= 0.99999) // the sky
        return;
    float surface = toWorldX.y / (d + toWorldX.x);
    if (abs(clip.w - surface) > tolerance)
        return;
    float3 c = colourTex.SampleLevel(pointClamp, uv, 0).rgb;
    if (!Finite3(c))
        return;
    uint old;
    InterlockedMax(occupancy[id], 256u + 253u, old);
    float4 v = voxels[id];
    float trust = saturate(kColourTrust / clip.w); // far: mostly fog and haze (as the screen's colour)
    c = clamp(c, 0, 8);
    v.rgb = v.a > 0.01 ? lerp(v.rgb, c, 0.5 * trust) : c * trust;
    v.a = max(v.a, 253.0 / 255.0);
    voxels[id] = v;
    seen[id] = seen[id] | 1u;
}

// Cover above per cell of level 0 at half its resolution (roofOut, the GI's RoofAt): five rays up (straight and four
// leaning 25 degrees) from the centre of the 2x2x2 block through level 0's cells within 32 cells (8 m), each covered
// by the opacity it meets (1 - the product of 1 - a); the mean. An eighth of the texture a frame (mode: the slab of z).
// (The geometry pass's opacity byte, which would tell leaves from roofs, read as 0 here: to look into.)
[numthreads(4, 4, 4)]
void roof(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w, half = n / 2;
    uint3 t = uint3(id.xy, id.z + (uint)mode * (uint)(half / 8));
    if (any(t >= (uint)half))
        return;
    int3 g = (int3)gridMin.xyz;
    int3 cell = g + Wrap(int3(t) * 2 - g); // the block's first cell in the grid now
    static const float3 kRays[5] = {float3(0, 0, 1), float3(0.42, 0, 0.91), float3(-0.42, 0, 0.91), float3(0, 0.42, 0.91), float3(0, -0.42, 0.91)};
    float stopped = 0;
    [loop] for (int r = 0; r < 5; r++)
    {
        float3 p = cell + 1 + kRays[r] * 1.5;
        float covered = 0;
        [loop] for (int k = 0; k < 32 && covered < 0.95; k++)
        {
            int3 c = (int3)floor(p);
            if (any(c < g) || any(c >= g + n))
                break;
            covered += (1 - covered) * voxels[Wrap(c)].a;
            p += kRays[r];
        }
        stopped += covered;
    }
    roofOut[t] = stopped / 5;
}

// One mip of the grid from the one below (u0 the level below, u2 this level; mode = this level). Toroidal addressing
// carries over: the children of texel t are 2t + (0..1). Level 1 premultiplies level 0's colour by its opacity, the
// cones sample levels 1 and up (premultiplied colour, mean opacity).
[numthreads(4, 4, 4)]
void downsample(uint3 id : SV_DispatchThreadID)
{
    uint n = (uint)gridMin.w >> (uint)mode;
    if (any(id >= n))
        return;
    float4 sum = 0;
    float a[8];
    [unroll] for (int j = 0; j < 8; j++)
    {
        float4 v = voxels[id * 2 + uint3(j & 1, (j >> 1) & 1, j >> 2)];
        if (mode == 1)
            v.rgb *= v.a;
        sum += v;
        a[j] = v.a;
    }
    sum /= 8;
    // The opacity as how much of a ray the block stops (worldToView[0].w, CRYMT_GI_MIPOPACITY): along each axis the mean
    // over its four columns of 1 - (1 - a near) (1 - a far); 1 the mean of the three axes, 2 the largest, 3 the largest
    // where it says wall (a full layer: 1) and the mean where it says scattered (a lone cell 0.25: foliage, clutter stay
    // as thin as before), blended between 0.5 and 1; 4 per axis (below). 0: the plain mean, which thins a one-cell wall
    // to half per mip (the cones saw through walls and roofs a few metres on).
    float mipOpacity = worldToView[0].w;
    if (mipOpacity > 3)
    {
        // 4: the opacity per axis in its own texture (xyz; the cones weight it by their direction) and the plain mean
        // in the grid (the light's weight, the roof pass at mip 0, the views). From cells (grid mip 1) the material
        // opacities are independent: a column stops 1 - (1 - a) (1 - b); from coverages (mip 2 and up) a wall's halves
        // line up: the column stops what its more covered half does, so a floor or the ground stays as thin for a ray
        // along it as the mean says and a wall stops a ray across it.
        float3 d[8];
        [unroll] for (int j2 = 0; j2 < 8; j2++)
            d[j2] = mode == 1 ? a[j2].xxx : dirSrc[id * 2 + uint3(j2 & 1, (j2 >> 1) & 1, j2 >> 2)].xyz;
        float3 o = 0;
        [unroll] for (int k = 0; k < 4; k++)
        {
            int jx = (k & 1) * 2 + (k >> 1) * 4, jy = (k & 1) + (k >> 1) * 4, jz = k; // the columns' first cells
            if (mode == 1)
                o += 1 - (1 - float3(d[jx].x, d[jy].y, d[jz].z)) * (1 - float3(d[jx + 1].x, d[jy + 2].y, d[jz + 4].z));
            else
                o += max(float3(d[jx].x, d[jy].y, d[jz].z), float3(d[jx + 1].x, d[jy + 2].y, d[jz + 4].z));
        }
        dirDst[id] = float4(o / 4, 0);
    }
    else if (mipOpacity > 0)
    {
        float ax = 0, ay = 0, az = 0;
        [unroll] for (int k = 0; k < 4; k++)
        {
            int jx = (k & 1) * 2 + (k >> 1) * 4, jy = (k & 1) + (k >> 1) * 4, jz = k; // the columns' first cells
            ax += 1 - (1 - a[jx]) * (1 - a[jx + 1]);
            ay += 1 - (1 - a[jy]) * (1 - a[jy + 2]);
            az += 1 - (1 - a[jz]) * (1 - a[jz + 4]);
        }
        float most = max(ax, max(ay, az)) / 4;
        if (mipOpacity > 2)
            sum.a = lerp(sum.a, most, saturate(most * 2 - 1));
        else
            sum.a = mipOpacity > 1 ? most : (ax + ay + az) / 12;
    }
    voxelsDst[id] = sum;
    if (voxFlags & 1) // the light direction: the children's, weighted by their light (CRYMT_GI_DIRLIGHT 1)
    {
        float3 gs = 0;
        float ws = 0;
        [unroll] for (int j3 = 0; j3 < 8; j3++)
        {
            uint3 cid = id * 2 + uint3(j3 & 1, (j3 >> 1) & 1, j3 >> 2);
            float4 cv = voxels[cid];
            float w = Luma(cv.rgb) * (mode == 1 ? cv.a : 1);
            gs += l1Grid[cid].xyz * w;
            ws += w;
        }
        l1Dst[id] = float4(ws > 1e-6 ? gs / ws : 0, 0);
    }
    // The six directions: premultiplied means, as the colour. (An anisotropic front-to-back composite per face was tried
    // 2026-11-14: the frame went brighter and the stair's 10 -> 15 h change grew from +6.8 to +19: not this way.)
    if (voxFlags & 2)
        [unroll] for (int f = 0; f < 6; f++)
        {
            float3 s6 = 0;
            [unroll] for (int j4 = 0; j4 < 8; j4++)
                s6 += SixLoad(f, id * 2 + uint3(j4 & 1, (j4 >> 1) & 1, j4 >> 2)) * (mode == 1 ? a[j4] : 1);
            SixDstStore(f, id, s6 / 8);
        }
}

// The info panel (GI test keys), its rectangle in pixels in worldToPrevClip's first row (width 0: none): the frame
// darkened behind it, the text (coloured on black) added.
float3 Panel(uint2 id, float3 colour)
{
    float4 rect = worldToPrevClip[0];
    if (rect.z > 0 && all(id >= (uint2)rect.xy) && all(id < (uint2)(rect.xy + rect.zw)))
    {
        float3 text = depthTex.Load(int3(id - (uint2)rect.xy, 0)).rgb;
        float cover = max(text.r, max(text.g, text.b));
        colour = colour * 0.35 * (1 - cover) + text;
    }
    return colour;
}

// Debug view over the finished frame: the cells a camera ray meets first, as wire faces (mode 1, red: level 0) or solid
// in their colour (mode 2); top left the GI as lighting only (indirect light plus the share of the flat ambient kept).
// Mode 0: the info panel alone.
[numthreads(8, 8, 1)]
void view(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float2 uv = (id.xy + 0.5) / fullSize;
    float4 result = sceneTex.Load(int3(id.xy, 0));
    if (mode < 1)
    {
        viewOut[id.xy] = float4(Panel(id.xy, result.rgb), 1);
        return;
    }
    const float inset = 0.4;
    if (uv.x < inset && uv.y < inset)
    {
        float4 gi = colourTex.SampleLevel(linearClamp, uv / inset, 0);
        float share = saturate(gi.a - 2 * floor(gi.a * 0.5)); // a = share + 2 x the shader patch's flags
        viewOut[id.xy] = float4(Panel(id.xy, sqrt(Tone(gi.rgb + (1 - share) * 0.5))), 1);
        return;
    }
    float3 dirV = normalize(float3((uv.x * 2 - 1) * invProj.x, (1 - uv.y * 2) * invProj.y, -1));
    float3 dir = float3(dot(dirV, toWorldX.xyz), dot(dirV, toWorldY.xyz), dot(dirV, toWorldZ.xyz));
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    int n = (int)gridMin.w;
    float travelled = 0; // metres from the eye: each level goes on where the finer one ended
    bool found = false;
    [loop] for (int level = 0; level < 4 && !found; level++)
    {
        float vs = voxelSize * (1 << level);
        int3 g = level == 0 ? (int3)gridMin.xyz : (int3)worldToPrevClip[level].xyz;
        float3 origin = (eye + dir * travelled) / vs; // in cells
        int3 cell = (int3)floor(origin);
        int3 stepDir = int3(dir.x >= 0 ? 1 : -1, dir.y >= 0 ? 1 : -1, dir.z >= 0 ? 1 : -1);
        float3 tDelta = 1 / max(abs(dir), 1e-6);
        float3 tMax = (dir >= 0 ? (cell + 1 - origin) : (origin - cell)) * tDelta;
        float t = 0;
        int axis = -1;
        [loop] for (int i = 0; i < 300; i++)
        {
            if (any(cell < g) || any(cell >= g + n))
                break;
            float4 v = level == 0 ? voxels[Wrap(cell)] : level == 1 ? voxels1[Wrap(cell)] : level == 2 ? voxels2[Wrap(cell)] : voxels3[Wrap(cell)];
            if (v.a > 0.5)
            {
                float3 hit = origin + dir * t - cell;
                float2 f = axis == 0 ? hit.yz : axis == 1 ? hit.xz : hit.xy;
                float2 e = axis < 0 ? 0.5 : min(f, 1 - f);
                float width = max((travelled + t * vs) * 2 * invProj.y / fullSize.y / vs, 0.01) * 1.5; // about 1.5 pixels, in cells
                bool edge = min(e.x, e.y) < width;
                float3 tint = level == 0 ? float3(1, 0, 0) : level == 1 ? float3(0, 1, 0) : level == 2 ? float3(0.2, 0.4, 1) : float3(1, 0.2, 1);
                if (mode >= 2)
                    result = float4(sqrt(Tone(v.rgb)) * (axis == 2 ? 1.0 : axis == 0 ? 0.85 : 0.7) * (edge ? 0.5 : 1), 1);
                else if (edge)
                    result = float4(tint, 1);
                found = true;
                break;
            }
            if (tMax.x < tMax.y && tMax.x < tMax.z)
            {
                cell.x += stepDir.x;
                t = tMax.x;
                tMax.x += tDelta.x;
                axis = 0;
            }
            else if (tMax.y < tMax.z)
            {
                cell.y += stepDir.y;
                t = tMax.y;
                tMax.y += tDelta.y;
                axis = 1;
            }
            else
            {
                cell.z += stepDir.z;
                t = tMax.z;
                tMax.z += tDelta.z;
                axis = 2;
            }
        }
        travelled += t * vs;
    }
    viewOut[id.xy] = float4(Panel(id.xy, result.rgb), 1);
}
)CRYMT"
