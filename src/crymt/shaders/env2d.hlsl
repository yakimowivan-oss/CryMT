// CryMT: the materials' 2D environment maps from the engine cube (RunEnv2D).
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kEnv2DShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
TextureCube<float4> cube : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState linearClamp : register(s0);
cbuffer C : register(b0)
{
  uint size;
  float lod;
};

[numthreads(8, 8, 1)]
void env2d(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= size || id.y >= size)
    return;
  float2 f = (id.xy + 0.5) / size;
  float3 c = 0;
  [unroll] for (int k = 0; k < 4; k++)
  {
    float2 xy = f - float2(k & 1, k >> 1);
    c += cube.SampleLevel(linearClamp, float3(xy, sqrt(saturate(1 - dot(xy, xy)))), lod).rgb * 0.25;
  }
  dst[id.xy] = float4(c, 1);
}
)CRYMT"
