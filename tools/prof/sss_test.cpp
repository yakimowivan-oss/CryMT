// Offline checks of CryMT's port of Bend Studio's screen-space shadows (src\crymt\BendSss.h), no game needed:
// 1. the compute shader compiles with d3dcompiler_47 as cs_5_0 (as the back end compiles it), for the sample counts given;
// 2. coverage: the CPU replays every dispatch of BuildDispatchList and the shader's ComputeWavefrontExtents for every
//    thread, and counts how often each pixel of the screen is written (each must be at least once) for suns on screen,
//    off screen, behind the camera, at the edges.
// Build: tools\llvm\bin\x86_64-w64-mingw32-clang++.exe -std=c++17 -O2 -I src\crymt tools\prof\sss_test.cpp -o sss_test.exe
// Run:   sss_test.exe [width height]
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "BendSss.h"

struct Macro
{
    const char* name;
    const char* def;
};
using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, void**, void**);
struct Blob // ID3DBlob's vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
{
    virtual HRESULT __stdcall QueryInterface(const IID&, void**) = 0;
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual SIZE_T __stdcall GetBufferSize() = 0;
};

static int CompileCheck(const char* samples)
{
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile)
    {
        printf("compile: no d3dcompiler_47\n");
        return 1;
    }
    Macro macros[] = {{"SAMPLE_COUNT", samples}, {nullptr, nullptr}};
    Blob* code = nullptr;
    Blob* errors = nullptr;
    const DWORD t0 = GetTickCount();
    HRESULT hr = compile(kBendSssShader, strlen(kBendSssShader), "CryMT_BendSss", macros, nullptr, "main", "cs_5_0", 1 << 15, 0,
                         reinterpret_cast<void**>(&code), reinterpret_cast<void**>(&errors));
    printf("compile SAMPLE_COUNT=%s: %s, %lu bytes, %lu ms\n", samples, SUCCEEDED(hr) ? "ok" : "FAILED", code ? static_cast<unsigned long>(code->GetBufferSize()) : 0ul,
           GetTickCount() - t0);
    if (errors)
        printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    return SUCCEEDED(hr) ? 0 : 1;
}

// The shader's ComputeWavefrontExtents, in float like the GPU.
static void Extents(const float lc[4], const int waveOffset[2], int gx, int gy, int gz, unsigned tid, float& px, float& py)
{
    const int W = 64;
    int xy[2] = {gy * W + waveOffset[0], gz * W + waveOffset[1]};
    float lx = floorf(lc[0]) + 0.5f, ly = floorf(lc[1]) + 0.5f;
    float fx = lc[0] - lx, fy = lc[1] - ly;
    bool reverse = lc[3] > 0;
    int sx = (xy[0] > 0) - (xy[0] < 0), sy = (xy[1] > 0) - (xy[1] < 0);
    bool horizontal = abs(xy[0] + sy) < abs(xy[1] - sx);
    int ax = horizontal ? sy : 0, ay = horizontal ? 0 : -sx;
    xy[0] += ax * gx;
    xy[1] += ay * gx;
    float xf = (float)xy[0], yf = (float)xy[1];
    bool xmajor = fabsf(xf) > fabsf(yf);
    float major = xmajor ? xf : yf;
    float mstart = fabsf(major), mend = fabsf(major) - (float)W;
    float mfrac = xmajor ? fx : fy;
    mfrac = major > 0 ? -mfrac : mfrac;
    float stx = xf + lx, sty = yf + ly;
    float t = (mend + mfrac) / (mstart + mfrac);
    float ex = lc[0] + (stx - lc[0]) * t, ey = lc[1] + (sty - lc[1]) * t;
    float step = (float)(tid ^ (reverse ? 0u : (unsigned)(W - 1)));
    px = stx + (ex - stx) * (step / (float)W);
    py = sty + (ey - sty) * (step / (float)W);
}

static bool Coverage(const char* name, float clip[4], int w, int h)
{
    int vp[2] = {w, h}, mn[2] = {0, 0}, mx[2] = {w, h};
    Bend::DispatchList list = Bend::BuildDispatchList(clip, vp, mn, mx, false, 64);
    std::vector<unsigned char> hits(static_cast<size_t>(w) * h, 0);
    long long threads = 0, inside = 0;
    for (int d = 0; d < list.DispatchCount; d++)
    {
        const Bend::DispatchData& dd = list.Dispatch[d];
        for (int gz = 0; gz < dd.WaveCount[2]; gz++)
            for (int gy = 0; gy < dd.WaveCount[1]; gy++)
                for (int gx = 0; gx < dd.WaveCount[0]; gx++)
                    for (unsigned t = 0; t < 64; t++)
                    {
                        float px, py;
                        Extents(list.LightCoordinate_Shader, dd.WaveOffset_Shader, gx, gy, gz, t, px, py);
                        threads++;
                        const int x = (int)floorf(px), y = (int)floorf(py);
                        if (x >= 0 && y >= 0 && x < w && y < h)
                        {
                            inside++;
                            if (hits[static_cast<size_t>(y) * w + x] < 255)
                                hits[static_cast<size_t>(y) * w + x]++;
                        }
                    }
    }
    long long missed = 0, multi = 0;
    int mx0 = -1, my0 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const unsigned char c = hits[static_cast<size_t>(y) * w + x];
            if (!c)
            {
                if (mx0 < 0)
                    mx0 = x, my0 = y;
                missed++;
            }
            else if (c > 1)
                multi++;
        }
    printf("%-28s light (%.1f, %.1f) w %+.0f: %d dispatches, %lld threads (%.2f x the pixels), on screen %lld; pixels missed %lld, written twice+ %lld%s\n",
           name, list.LightCoordinate_Shader[0], list.LightCoordinate_Shader[1], list.LightCoordinate_Shader[3], list.DispatchCount, threads,
           (double)threads / ((double)w * h), inside, missed, multi, missed ? "  <-- MISSED" : "");
    if (missed)
        printf("    first missed pixel %d,%d\n", mx0, my0);
    return missed == 0;
}

int main(int argc, char** argv)
{
    const int w = argc > 2 ? atoi(argv[1]) : 2560, h = argc > 2 ? atoi(argv[2]) : 1440;
    int bad = CompileCheck("60") + CompileCheck("96");
    struct Case
    {
        const char* name;
        float clip[4];
    } cases[] = {
        {"on screen, centre", {0.0f, 0.0f, 0.0f, 1.0f}},
        {"on screen, off centre", {0.31f, -0.47f, 0.0f, 1.0f}},
        {"on screen, near corner", {0.98f, 0.97f, 0.0f, 1.0f}},
        {"off screen right", {3.0f, 0.2f, 0.0f, 1.0f}},
        {"off screen far up", {0.1f, 40.0f, 0.0f, 1.0f}},
        {"behind, centre", {0.05f, -0.1f, 0.0f, -1.0f}},
        {"behind, off screen", {-2.5f, 1.7f, 0.0f, -1.0f}},
        {"side (w ~ 0)", {0.7f, 0.3f, 0.0f, 1e-7f}},
        {"overhead (w ~ 0, -)", {0.0f, 0.9f, 0.0f, -3e-6f}},
    };
    for (Case& c : cases)
        bad += Coverage(c.name, c.clip, w, h) ? 0 : 1;
    printf(bad ? "FAILED: %d\n" : "all ok\n", bad);
    return bad;
}
