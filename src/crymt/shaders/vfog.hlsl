// CryMT: the GI in the fog: the froxel volume (RunVfog).
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kVfogShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
Texture2D<float4> pvolTex : register(t0);
RWTexture3D<float4> outVol : register(u0);
SamplerState linearClamp : register(s0);
cbuffer C : register(b0)
{
  float4 toWorldX; float4 toWorldY; float4 toWorldZ;
  float4 fog;  // x atmosphere scale, y density at the viewer, z density, w the artist's offset (VolumetricFog.cfi)
  float4 grid; // x, y tiles across and down, z slices, w reach (m)
  float4 misc; // xy 1 / proj._11, 1 / proj._22; z the GI's strength in the fog; w the mode
  float4 misc2; // x the fog's own darkening outdoors against indoors (the particles' is apart), y the share's cap, z the
                // sun shadows' strength (stage 2)
  float4 misc3; // the near air: x its density (log2 units a metre), y its reach (m)
};
// The fog pass's parameters (its CryMT_VFogParamSampler slot): x, y the near air, w 4242 (the mark: CryMT's volume this
// frame; without it the patched fog pass keeps the engine's fog).
RWTexture2D<float4> paramOut : register(u1);

// The near air's depth along a ray to t (as the patched fog pass's CryMT_HazeDepth): full to reach / 2, then fading.
float HazeDepth(float t, float reach)
{
  float b = clamp(t - 0.5 * reach, 0, 0.5 * reach);
  return min(t, 0.5 * reach) + b - b * b / max(reach, 0.001);
}
// Stage 2: the sun's shadow cascades (NoteSunMap), finest first: light matrices for positions from the camera (row
// vectors; xy / w the map's NDC, z the depth as the map stores it: linear over the light's far distance, a little bias
// in), count in x.
Texture2D<float> sunMaps[4] : register(t1);
SamplerComparisonState shadowCmp : register(s1);
cbuffer Cascades : register(b1)
{
  row_major float4x4 cascade[4];
  float4 cascadeInfo;
};

// The sun's visibility at pos (from the camera) from the finest cascade holding it (2x2 compared, filtered); -1 outside.
float SunShadow(float3 pos)
{
  [unroll] for (int c = 0; c < 4; c++)
  {
    if (c < (int)cascadeInfo.x)
    {
      float4 clip = mul(float4(pos, 1), cascade[c]);
      if (clip.w > 0)
      {
        float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
        if (all(uv > 0.01) && all(uv < 0.99) && clip.z > 0 && clip.z < 1)
          return sunMaps[c].SampleCmpLevelZero(shadowCmp, uv, clip.z);
      }
    }
  }
  return -1;
}

// ComputeVolumetricFog's exponent: the fog seen through the ray c is exp2(-max(x, 0)).
float FogExponent(float3 c)
{
  float fogInt = 1;
  if (abs(c.z) > 0.01)
  {
    float t = fog.x * c.z;
    fogInt *= (1.0 - exp(-t)) / t;
  }
  return fogInt * length(c) * fog.y - fog.w;
}

// One of the particles' volumes (as Particles.cfi's CryMT_PVolOne reads pvolume's atlas): its share, sun, indoor share
// and share before the outdoor part, its tint, its weight (0 outside, rising over the outer 4 cells).
void PVolOne(float3 pos, int p, float x0, out float4 v, out float3 tint, out float weight)
{
  v = 0;
  tint = 1;
  weight = 0;
  float cell = pvolTex.Load(int3(p + 3, 256, 0)).x;
  if (cell <= 0)
    return;
  float3 first = float3(pvolTex.Load(int3(p, 256, 0)).x, pvolTex.Load(int3(p + 1, 256, 0)).x, pvolTex.Load(int3(p + 2, 256, 0)).x);
  float3 dims = float3(64, 64, 32);
  float3 g = (pos - first) / cell - 0.5;
  float edge = min(min(min(g.x, dims.x - 1 - g.x), min(g.y, dims.y - 1 - g.y)), min(g.z, dims.z - 1 - g.z));
  if (edge < 0)
    return;
  float z0 = floor(g.z);
  float z1 = min(z0 + 1, dims.z - 1);
  float fz = g.z - z0;
  float2 xy = clamp(g.xy + 0.5, 0.5, dims.xy - 0.5);
  float2 uv0 = (float2(x0, 0) + float2(fmod(z0, 8), floor(z0 / 8)) * dims.xy + xy) / float2(1024, 513);
  float2 uv1 = (float2(x0, 0) + float2(fmod(z1, 8), floor(z1 / 8)) * dims.xy + xy) / float2(1024, 513);
  float2 down = float2(0, 257.0 / 513);
  v = lerp(pvolTex.SampleLevel(linearClamp, uv0, 0), pvolTex.SampleLevel(linearClamp, uv1, 0), fz);
  tint = lerp(pvolTex.SampleLevel(linearClamp, uv0 + down, 0).xyz, pvolTex.SampleLevel(linearClamp, uv1 + down, 0).xyz, fz);
  weight = saturate(edge / 4);
}

// The fog's own share of the ambient lost (the particles' x has their outdoor part): w with misc2's outdoor part, capped.
float FogShare(float4 v)
{
  return min(saturate(v.w) * lerp(misc2.x, 1, saturate(v.z)), misc2.y);
}

// The fog's light at pos (from the camera, metres): rgb what is left of the ambient part (tinted), a of the sun's part
// (the voxels' sun by the indoor share, as for particles: outdoors their grass and coarse ground cells shaded the air).
float4 Visibility(float3 pos)
{
  float4 fv, cv;
  float3 ft, ct;
  float fw, cw;
  PVolOne(pos, 0, 0, fv, ft, fw);
  PVolOne(pos, 4, 512, cv, ct, cw);
  float share = lerp(FogShare(cv) * cw, FogShare(fv), fw);
  float3 tint = clamp(lerp(lerp(1, ct, cw), ft, fw), 0, 2);
  float cs = saturate(cv.z) * cw, fs = saturate(fv.z) * fw;
  float sun = fs > 0 ? saturate(fv.y) : saturate(cv.y);
  float sunWeight = (fv.y < -0.5 || cv.y < -0.5) ? 0 : max(fs, cs);
  float k = saturate(misc.z);
  return float4((1 - saturate(share * misc.z)) * lerp(1, tint, k), lerp(1, sun, sunWeight * k));
}

// Stage 2's sub-samples of the sun's shadow per cell (misc3.z: 1, 4 or 8): offsets in the tile (xy, tiles) and in the
// slice (z, slices), the rotated grids of 4x and 8x MSAA. One sample a cell drew the shadows' edges as steps of whole
// tiles and slices (the user, Sponza 2026-11-15); a cell's share of samples in the shadow makes them a ramp.
static const float3 kSub4[4] = {float3(-0.125, -0.375, -0.375), float3(0.375, -0.125, 0.125), float3(-0.375, 0.125, -0.125),
                                float3(0.125, 0.375, 0.375)};
static const float3 kSub8[8] = {float3(0.0625, -0.1875, -0.4375), float3(-0.0625, 0.1875, 0.0625), float3(0.3125, 0.0625, -0.1875),
                                float3(-0.1875, -0.3125, 0.3125), float3(-0.3125, 0.3125, -0.3125), float3(-0.4375, -0.0625, 0.1875),
                                float3(0.1875, 0.4375, 0.4375), float3(0.4375, -0.4375, -0.0625)};
float3 SubOffset(int s, int count)
{
  return count >= 8 ? kSub8[s] : count >= 4 ? kSub4[s & 3] : float3(0, 0, 0);
}

[numthreads(8, 8, 1)]
void vfog(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= (uint)grid.x || id.y >= (uint)grid.y)
    return;
  if (id.x == 0 && id.y == 0)
    paramOut[int2(0, 0)] = float4(misc3.x, misc3.y, 0, 4242);
  int n = (int)grid.z;
  if (misc.w > 1.5)
  {
    [loop] for (int z = 0; z < n; z++)
      outVol[int3(id.xy, z)] = 0;
    return;
  }
  float2 uv = (id.xy + 0.5) / grid.xy;
  float3 dirV = normalize(float3((uv.x * 2 - 1) * misc.x, (1 - uv.y * 2) * misc.y, -1));
  float3 dir = normalize(float3(dot(dirV, toWorldX.xyz), dot(dirV, toWorldY.xyz), dot(dirV, toWorldZ.xyz)));
  // The sun's sub-samples' rays (stage 2).
  int ns = misc2.z > 0 ? clamp((int)misc3.z, 1, 8) : 0;
  float3 subDir[8];
  [unroll] for (int s = 0; s < 8; s++)
  {
    float2 su = (id.xy + 0.5 + SubOffset(s, ns).xy) / grid.xy;
    float3 sv = normalize(float3((su.x * 2 - 1) * misc.x, (1 - su.y * 2) * misc.y, -1));
    subDir[s] = normalize(float3(dot(sv, toWorldX.xyz), dot(sv, toWorldY.xyz), dot(sv, toWorldZ.xyz)));
  }
  // The fog's exponent along the ray: the engine's (its T = exp2(-max(x, 0)), non-decreasing) plus the near air's.
  float xPrev = 0; // at the camera no fog yet (a negative artist offset's fog at once falls into the first slice)
  float xEngine = 0;
  float4 acc = 0;
  float wsum = 0;
  [loop] for (int k = 0; k < n; k++)
  {
    float e = (k + 1.0) / n, m = (k + 0.5) / n;
    float t1 = grid.w * e * e;
    xEngine = max(FogExponent(dir * t1), xEngine);
    float x1 = max(xEngine + misc3.x * HazeDepth(t1, misc3.y), xPrev);
    float d = x1 - xPrev;
    // What the slice adds to the fog seen through it, T(start) - T(end), written so tiny steps keep their precision.
    float w = exp2(-xPrev) * (d < 0.001 ? d * 0.6931472 * (1 - d * 0.3465736) : 1 - exp2(-d));
    float4 v = Visibility(dir * (grid.w * m * m));
    // Stage 2: the sun's part from the shadow maps, its sub-samples averaged (one outside the cascades takes the
    // stage 1 rule's value).
    if (ns > 0)
    {
      float sum = 0;
      [unroll] for (int s = 0; s < 8; s++)
      {
        if (s < ns)
        {
          float3 o = SubOffset(s, ns);
          float ms = (k + 0.5 + o.z) / n;
          float sh = SunShadow(subDir[s] * (grid.w * ms * ms));
          sum += sh >= 0 ? lerp(1, sh, misc2.z) : v.a;
        }
      }
      v.a = sum / ns;
    }
    acc += w * v;
    wsum += w;
    xPrev = x1;
    outVol[int3(id.xy, k)] = 1 - (wsum > 1e-20 ? acc / wsum : v);
  }
}
)CRYMT"
