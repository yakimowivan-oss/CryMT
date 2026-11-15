// CryMT: the engine cube's HDR copy, mips and prefiltered copies (RunCubeUpdate).
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kCubeShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
Texture2DArray<float4> ldr : register(t0);
TextureCube<float4> prev : register(t1);
SamplerState linearClamp : register(s0);
RWTexture2DArray<float4> dst : register(u0);
RWTexture2DArray<float4> dst2 : register(u1);
cbuffer C : register(b0)
{
  uint size;
  uint face;
  float blend;
  float k; // tan of the engine faces' half field of view (1: 90 degrees)
  float alpha;    // cubePrefilter: the level's GGX roughness (alpha = r^2, r = level / 6)
  float scaleAll; // ... the materials' copies: every material's (dst), those with their own map (dst2)
  float scaleOwn;
  uint size0;     // ... the HDR copy's level-0 size
};

// The direction of a D3D cube face's point (s, t in -1..1, t down).
float3 CubeDir(uint f, float s, float t)
{
  return f == 0 ? float3(1, -t, -s) : f == 1 ? float3(-1, -t, s) : f == 2 ? float3(s, 1, t) : f == 3 ? float3(s, -1, -t)
       : f == 4 ? float3(s, -t, 1) : float3(-s, -t, -1);
}

float3 Ldr(uint f, int2 p)
{
  float4 c = ldr.Load(int4(p, f, 0));
  return c.rgb * (c.a * 16);
}

// Bilinear in the engine's face g at (s, t) of its own (wide) view; its image is flipped vertically.
float3 SampleFace(uint g, float2 st)
{
  float2 p = (float2(st.x, -st.y) * 0.5 + 0.5) * size - 0.5;
  int2 p0 = (int2)floor(p);
  float2 w = p - p0;
  int2 lo = clamp(p0, 0, (int)size - 1), hi = clamp(p0 + 1, 0, (int)size - 1);
  return lerp(lerp(Ldr(g, lo), Ldr(g, int2(hi.x, lo.y)), w.x), lerp(Ldr(g, int2(lo.x, hi.y)), Ldr(g, hi), w.x), w.y);
}

[numthreads(8, 8, 1)]
void cubeDecode(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= size || id.y >= size)
    return;
  float2 st = (id.xy + 0.5) / size * 2 - 1;
  float3 d = CubeDir(id.z, st.x, st.y);
  float3 hdr = 0;
  if (k <= 1.0001)
    hdr = SampleFace(id.z, st);
  else
  {
    // Every face whose wide view holds d, weighted by how far inside: 1 within its 90 degrees, down to 0 at its view's
    // edge; on the 90-degree edge two faces weigh the same.
    float margin = 1 - 1 / k, sum = 0;
    for (uint g = 0; g < 6; g++)
    {
      float3 a = g == 0 ? float3(d.x, -d.z, -d.y) : g == 1 ? float3(-d.x, d.z, -d.y) : g == 2 ? float3(d.y, d.x, d.z)
               : g == 3 ? float3(-d.y, d.x, -d.z) : g == 4 ? float3(d.z, d.x, -d.y) : float3(-d.z, -d.x, -d.y);
      if (a.x <= 0)
        continue;
      float2 sg = a.yz / (a.x * k);
      float w = saturate((1 - max(abs(sg.x), abs(sg.y))) / margin);
      if (w > 0)
      {
        hdr += w * SampleFace(g, sg);
        sum += w;
      }
    }
    hdr /= max(sum, 1e-5);
  }
  float3 old = blend < 1 ? dst[id].rgb : hdr;
  dst[id] = float4(lerp(old, hdr, blend), 1);
}

float RadicalInverse(uint b)
{
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  return float(b) * 2.3283064365386963e-10;
}

// The materials' copies, one level: the HDR copy (prev, all its levels) convolved with GGX of this level's roughness
// (32 samples by importance, each read at the level its solid angle covers: filtered importance sampling; the view
// along the normal), then encoded as the voxel cube (1 - exp(-0.5 x)) with each copy's scale. Level 0: the mirror.
[numthreads(8, 8, 1)]
void cubePrefilter(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= size || id.y >= size)
    return;
  float2 st = (id.xy + 0.5) / size * 2 - 1;
  float3 n = normalize(CubeDir(id.z, st.x, st.y));
  float3 c;
  if (alpha <= 0)
    c = prev.SampleLevel(linearClamp, n, 0).rgb;
  else
  {
    float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n));
    float3 ty = cross(n, tx);
    float a2 = alpha * alpha;
    float texelSa = 4 * 3.14159265 / (6.0 * size0 * size0);
    float3 sum = 0;
    float weight = 0;
    for (uint i = 0; i < 32; i++)
    {
      float2 xi = float2((i + 0.5) / 32, RadicalInverse(i));
      float phi = 6.2831853 * xi.x;
      float cosT = sqrt((1 - xi.y) / (1 + (a2 - 1) * xi.y));
      float sinT = sqrt(1 - cosT * cosT);
      float3 h = tx * (sinT * cos(phi)) + ty * (sinT * sin(phi)) + n * cosT;
      float3 l = 2 * dot(n, h) * h - n;
      float nl = dot(n, l);
      if (nl > 0)
      {
        float d = cosT * cosT * (a2 - 1) + 1;
        float pdf = a2 / (3.14159265 * d * d) / 4; // D(h) n.h / (4 v.h), v = n
        float lod = max(0.5 * log2(1 / (32 * pdf + 1e-4) / texelSa) + 1, 0);
        sum += prev.SampleLevel(linearClamp, l, lod).rgb * nl;
        weight += nl;
      }
    }
    c = sum / max(weight, 1e-4);
  }
  dst[id] = float4(1 - exp(-0.5 * c * scaleAll), 1);
  dst2[id] = float4(1 - exp(-0.5 * c * scaleOwn), 1);
}

[numthreads(8, 8, 1)]
void cubeDown(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= size || id.y >= size)
    return;
  float2 st = (id.xy + 0.5) / size * 2 - 1;
  dst[id] = prev.SampleLevel(linearClamp, CubeDir(id.z, st.x, st.y), 0);
}
)CRYMT"
