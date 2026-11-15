// CryMT: the water SSR's depth pyramid (RunHiz).
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kHizShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
Texture2D<float> zTex : register(t0);
Texture2D<float> hwDepth : register(t1);
RWTexture2D<float> dst : register(u0);
RWTexture2D<float> src : register(u1);
cbuffer C : register(b0)
{
  uint2 dstSize;
  uint2 srcSize;
  float nearOverFar;
  float hasHw;
};

[numthreads(8, 8, 1)]
void hizInit(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= dstSize.x || id.y >= dstSize.y)
    return;
  float L = zTex.Load(int3(id.xy, 0));
  float d = L > 0 && L < 0.999 ? nearOverFar / L : 0;
  if (hasHw > 0.5 && hwDepth.Load(int3(id.xy, 0)) < 0.1)
    d = 0;
  dst[id.xy] = d;
}

[numthreads(8, 8, 1)]
void hizDown(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= dstSize.x || id.y >= dstSize.y)
    return;
  uint2 s = id.xy * 2;
  uint2 last = srcSize - 1;
  float d = max(max(src[min(s, last)], src[min(s + uint2(1, 0), last)]), max(src[min(s + uint2(0, 1), last)], src[min(s + 1, last)]));
  // An odd source: its last column / row goes into the last texel.
  if ((srcSize.x & 1) && id.x == dstSize.x - 1)
    d = max(d, max(src[min(uint2(last.x, s.y), last)], src[min(uint2(last.x, s.y + 1), last)]));
  if ((srcSize.y & 1) && id.y == dstSize.y - 1)
    d = max(d, max(src[min(uint2(s.x, last.y), last)], src[min(uint2(s.x + 1, last.y), last)]));
  dst[id.xy] = d;
}
)CRYMT"
