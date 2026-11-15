// CryMT: the scene depth for the GI from a viewport of a larger target.
// A C++ raw string literal: D3D12Backend.cpp / BendSss.h include it as kDepthAlignShader (tools\prof\shader_check.py compiles it with fxc).
R"CRYMT(
cbuffer DepthConstants : register(b0)
{
    float2 outputSize;
    float2 sourceOrigin;
    float2 sourceSize;
};
Texture2D<float> sourceDepth : register(t0);
RWTexture2D<float> outputDepth : register(u0);
[numthreads(8, 8, 1)]
void AlignDepth(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)outputSize.x || id.y >= (uint)outputSize.y)
        return;
    uint width, height;
    sourceDepth.GetDimensions(width, height);
    float2 p = sourceOrigin + (id.xy + 0.5) * sourceSize / outputSize;
    int2 pixel = clamp((int2)floor(p), int2(0, 0), int2(width - 1, height - 1));
    outputDepth[id.xy] = sourceDepth.Load(int3(pixel, 0));
}
)CRYMT"
