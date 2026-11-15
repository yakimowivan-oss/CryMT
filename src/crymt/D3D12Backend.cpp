// D3D12 back end (CRYMT_D3D12=1). The renderer's final D3D10 device, every object it creates through it and its swap
// chain are implemented on D3D12; the probe devices it creates first stay real D3D10.
// The immediate context is translated into one direct command list per submission. State is tracked D3D10-style and
// applied at each draw: pipeline state objects cached by state ids, root CBVs for the constant buffer slots the shaders
// declare, SRV tables built in a shader-visible descriptor ring, sampler tables cached by sampler set. Textures carry their
// D3D12 state and get barriers when their use changes; D3D10's binding hazard rules (a resource bound for output is
// unbound from shader inputs) are kept. Dynamic buffers rename into upload memory on Map(DISCARD); default buffers are
// sub-allocated from default-heap pages; creation uploads go through their own list, executed before the main list.
// Main lists are numbered (serials); objects released by the renderer are freed once the list being recorded at that
// time has completed on the GPU.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS // MSVC ABI for COM methods that return structures
#include <windows.h>
#include <d3d10_1.h>
#include <d3d10shader.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "D3D12Backend.h"
#include "Dlss.h"
#include "Paths.h"
#include "DrawPacket.h"
#include "Window.h"
#include "GiControl.h"
#include "RenderDocCapture.h"
#include "BendSss.h"
#include "ZPass.h"

// (The contact shadows' frame-to-frame mix, CRYMT_SSS_TEMPORAL, made for the test camera's drift taken for flicker,
// was removed 2026-11-15: tag experiments-2026-11-15.)

#include <string>

uint64_t CryMTWorldFrame(); // CryMT.cpp: world frames since start
void CryMTConsoleCommand(const char* command); // CryMT.cpp, main thread: the panel's engine settings
void CryMTSetTimeOfDay(float hours);           // CryMT.cpp, main thread: the panel's time of day
GiControl g_giControl;

namespace dx12
{
namespace
{
#include "d3d12/infra.inc"
template <class I>
class Child : public I
{
public:
    Child() : m_id(g_nextId.fetch_add(1, std::memory_order_relaxed)) {}
    virtual ~Child() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (Supports(riid))
        {
            this->AddRef();
            *out = static_cast<I*>(this);
            return S_OK;
        }
        *out = nullptr;
        LogGuid("device child", riid);
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (refs == 0)
            Destroy();
        return refs;
    }

    void STDMETHODCALLTYPE GetDevice(ID3D10Device** out) override
    {
        if (out)
            *out = DeviceInterface();
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        return PrivateGet(this, guid, size, data);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        m_private = true;
        return PrivateSet(this, guid, size, data, nullptr);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
    {
        m_private = true;
        return PrivateSet(this, guid, 0, nullptr, data);
    }

    // For the creation caches: takes a reference unless the object is already being destroyed.
    bool TryAddRef()
    {
        ULONG refs = m_refs.load(std::memory_order_relaxed);
        while (refs)
            if (m_refs.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed))
                return true;
        return false;
    }

    uint32_t Id() const
    {
        return m_id;
    }

protected:
    virtual bool Supports(REFIID riid) const
    {
        return IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(ID3D10DeviceChild)) || IsEqualIID(riid, __uuidof(I));
    }

    virtual void Destroy()
    {
        if (m_private)
            PrivateErase(this);
        delete this;
    }

    std::atomic<ULONG> m_refs{1};
    uint32_t m_id;
    bool m_private = false;
};

// What views, copies and the state tracker need from any resource.
struct ResourceInfo
{
    bool buffer = false;
    ID3D12Resource* res = nullptr;                             // textures; staging buffers
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON; // textures
    int outputBound = 0;                                       // bound as a render target or depth stencil now
    uint64_t gpuWriteSerial = 0, gpuReadSerial = 0;            // staging: last main lists copying into / out of it
    uint8_t* stagingCpu = nullptr;
    bool envCube = false; // a material's environment cube map (see IsEnvCube)
};

ResourceInfo* Info(ID3D10Resource* r);
uint32_t EnvTestColour(); // CRYMT_ENVTEST (see IsEnvCube)

// Resources only water and see-through surfaces read: their environment maps stay the game's. Water.cfx, WaterOcean.cfx
// and WaterVortex.cfx read envMapSamplerRefl/Refr, but the compiled WaterSurface.cfx / WaterVols.cfx variants of some
// rivers (river_village.mtl, river_harbor.mtl) bind only envMapSamplerCUBE next to the screen, foam or gloss maps, and
// took CryMT's voxel cube (the user, 2026-11-11: the rivers of village and harbor overbright). screenMapSampler (the
// screen behind) is also read by glass, ice and Methagen, which keep their maps as well.
bool IsRefractiveResource(const char* name)
{
    static const char* const kNames[] = {"envMapSamplerRefl", "envMapSamplerRefr", "screenMapSampler", "foamSampler", "foamSamplerSec",
                                         "waterGlossMapSampler", "waterVolumeHeightSampler", "waveSampler", "waveBumpSampler",
                                         "fftTexSampler", "oceanBumpMapSampler"};
    for (const char* n : kNames)
        if (strcmp(name, n) == 0)
            return true;
    return false;
}
const ResourceInfo* g_giPlaceholder; // Textures/CryMT/gi.dds, see IsGiPlaceholder
const ResourceInfo* g_jitterPlaceholder; // Textures/CryMT/jitter.dds, see IsJitterPlaceholder
const ResourceInfo* g_foliagePlaceholder; // Textures/CryMT/foliage.dds, see IsFoliagePlaceholder
const ResourceInfo* g_pvolPlaceholder;    // Textures/CryMT/pvol.dds, see IsPvolPlaceholder
const ResourceInfo* g_fplusPlaceholder;   // Textures/CryMT/fplus.dds, see IsFPlusPlaceholder
const ResourceInfo* g_sssPlaceholder;     // Textures/CryMT/sss.dds (ShadowMaskGen's CryMT_SSSSampler), see IsSssPlaceholder
const ResourceInfo* g_envPlaceholder;     // Textures/CryMT/env.dds (fragLib's CryMT_EnvSampler), see IsEnvPlaceholder
const ResourceInfo* g_waterEnvPlaceholder; // Textures/CryMT/waterenv.dds (Water.cfx's CryMT_WaterEnvSampler), see IsWaterEnvPlaceholder
const ResourceInfo* g_hizPlaceholder;      // Textures/CryMT/hiz.dds (Water.cfx's CryMT_HiZSampler), see IsHizPlaceholder
const ResourceInfo* g_ssrPrevPlaceholder;  // Textures/CryMT/ssrprev.dds (Water.cfx's CryMT_WaterPrevSampler), see IsSsrPrevPlaceholder
const ResourceInfo* g_ssrPartPlaceholder;  // Textures/CryMT/ssrpart.dds (Water.cfx's CryMT_WaterPartSampler), see IsSsrPartPlaceholder
const ResourceInfo* g_vfogPlaceholder;     // Textures/CryMT/vfog.dds (PostProcess.cfx's CryMT_VFogSampler), see IsVfogPlaceholder

// The 2D environment map from the engine's cube (Device::RunEnv2D; the first-person weapons' Metal reads its own 2D map
// by reflection.xy, not a cube: in the engine cube mode it kept the game's static map while the voxel mode replaced it,
// the user 2026-11-15). As the voxel pass's square 6: a texel at f stands for the upward directions (xy, sqrt(1 - xy.xy))
// with xy = f and f - 1 per axis (the map wraps), the four averaged; from the materials' cube (GGX by level, the panel's
// strength in) at level `lod`. Constants: the map's size, the level.
const char* const kEnv2DShader =
#include "shaders/env2d.hlsl"
    ;

// Volumetric fog, stage 1: the GI in the fog (Device::RunVfog, 2026-11-14). The engine's fog (PostProcess.cfx FogPass)
// lights all air alike, so it glowed in dark rooms and in the shade. Per screen tile (a thread) the ray from the camera
// through the tile's centre, slices to `reach` metres (slice k ends at reach ((k + 1) / n)^2): each slice's share of the
// fog's light by the engine's own formula (ComputeVolumetricFog with its vfParams: what the slice adds to the fog seen
// through it, T(start) - T(end)) and the light's visibility at its middle from the particles' GI volumes (pvolume: the
// ambient's share lost, tinted; the voxels' sun indoors, the engine's outdoors). Out per slice: 1 - the visibility
// averaged from the camera to the slice's end with those shares (rgb the ambient part, a the sun's part); the patched
// FogPass darkens the fog's colour by it up to `reach` and leaves the rest. Constants: the view to the world (rows, w the
// eye), the engine's vfParams, the grid (tiles across, down, slices, reach), the projection, the GI's strength in the fog
// and the mode (1 on, 2 zeros: the engine's fog, a check).
const char* const kVfogShader =
#include "shaders/vfog.hlsl"
    ;

// The depth pyramid for screen-space reflections (RunHiz): level 0 = near / view depth from the z-prepass's linear depth
// (w / far), 0 for the sky and the first-person view (the hardware depth under 0.1: rays pass it); each further level the
// largest of 2x2 (inverted depth: the nearest). Constants: x, y the level's size, z, w the source level's size, near / far,
// 1 with the hardware depth.
const char* const kHizShader =
#include "shaders/hiz.hlsl"
    ;

// The engine's environment cube as the back end keeps it (Device::RunCubeUpdate): HDR, in D3D's cube layout, with all its
// mip levels. cubeDecode: one face from the engine's RGBK cube (rgb a 16), flipped vertically (the engine's face images
// are), mixed into the old content by `blend` (1 replaces); cubeDown: the next level of all six faces, each texel the
// level above looked up as a cube by the texel's direction (bilinear at the corner of 2x2: their mean; across the faces'
// edges the hardware's cube filtering takes the neighbour's texels, so the faces meet at every level: per-face 2x2 means
// left seams on far water). Constants: the written level's size, the face, the blend.
const char* const kCubeShader =
#include "shaders/cube.hlsl"
    ;
SerializeRootSignatureFn g_serializeRoot;

// CryMT GI compute pass (Device::RunGi). Root constants b0 (kGiConstants floats), t0 scene depth, t1 previous HDR colour,
// t2 previous GI, u0 new GI, u1/u2 this and the previous frame's distances; a second table t3 the voxel grid with its
// mips (kVoxShader); s0 point clamp, s1 linear clamp, s2 linear wrap (the grid is toroidal).
constexpr UINT kGiConstants = 62; // with the two tables 64 DWORDs, the root signature's limit
constexpr UINT kEnvSize = 64; // the environment maps' size (kGiShader envmap)
constexpr UINT kPvolX = 64, kPvolY = 64, kPvolZ = 32, kPvolTiles = 8; // the particles' occlusion volumes (two: 1024 x 257 atlas)
// The editor draws a viewport's scene into a colour target smaller than its depth target (the window's): the GI takes
// that viewport's depth resampled to the colour's size (SceneDepth). After a contributor's patch (support editor.patch,
// 2026-11-08).
const char* const kDepthAlignShader =
#include "shaders/depth_align.hlsl"
    ;

const char* const kGiShader =
#include "shaders/gi.hlsl"
    ;

// CryMT GI step 2, voxel grid (Device::RunVoxelUpdate / RunVoxelView; CRYMT_GI_VOXELS, CRYMT_GI_VOXELVIEW). A grid of
// N^3 cells of voxelSize metres around the camera, addressed toroidally (cell mod N), so moving the camera only resets
// the cells that entered the grid. Each frame every cell is projected onto the screen: in front of the visible surface it
// is empty space, at the surface it takes the lit colour seen there (the previous frame's HDR colour: albedo times all
// light, the GI included, which gives further bounces), behind it it keeps what it had. rgb lit colour, a opacity.
// Root constants b0 (kVoxConstants floats); t0 depth, t1 colour (view: the GI), t2 the finished frame, u0 the grid, u1
// the debug view; s0 point clamp, s1 linear clamp.
constexpr UINT kVoxConstants = 60;
constexpr int kVoxN = 128;
constexpr UINT kVoxMips = 5; // 128 .. 8
constexpr int kVoxLevels = 4;  // cells of voxelSize (0.25 m), 2, 4 and 8 times that: 32, 64, 128 and 256 m around the camera
// The GI root's second table (kGiShader t3..t41): per level the grid, per-axis opacity, L1 and six directions (9), the
// VisAreas, the roof, the probes' atlases (t41 light and states, t42 distances).
constexpr UINT kGiVoxTable = 9 * kVoxLevels + 5; // ... t41, t42 the probes' atlases, t43 the foliage mask
constexpr UINT kProbeCascades = 2; // GI v2's probe cascades (kGiShader's kProbeCascades: 0.5 and 1 m apart)
constexpr UINT kVoxTable = 23; // descriptors per table: t0..t2, u0..u19 (u6/u7 the light direction, u8..u19 the six
                               // directions: CRYMT_GI_DIRLIGHT)
const char* const kVoxShader =
#include "shaders/vox.hlsl"
    ;

// GI geometry (Device::RunVoxelGeometry): the frame's z-prepass draws again, with their own vertex shaders and inputs,
// but the camera's view-projection in their constant buffer replaced by an orthographic projection of the level-0 grid
// along one axis (x, y, z in turn, one a frame): the viewport is the grid's N x N face, depth runs through its N cells.
// This pixel shader stamps every cell a fragment falls into (toroidal, as the grid) with the frame and an opacity:
// stamp * 256 + opacity * 255. Foliage is drawn without its alpha test (whole leaf cards), so its cells count as mostly
// open (CRYMT_GI_FOLIAGE, as Crytek's VegetationMaxOpacity). Root: the main signature plus u0 (table, parameter kVoxGeomUav) and b13 (constants,
// kVoxGeomConstants).
constexpr UINT kVoxGeomUav = 26, kVoxGeomConstants = 27;
constexpr int kVoxGeomLevels = 3; // grid levels that get geometry (0.25, 0.5, 1 m cells: 32, 64, 128 m)
const char* const kVoxGeomShader =
#include "shaders/vox_geom.hlsl"
    ;

// The GI geometry pass with materials (Device::MaterialPs builds the input struct per vertex shader): as kVoxGeomShader,
// plus the material's texture: alpha-tested materials (job.w) leave texels under 0.5 out (leaves by their shape), and the
// texture's colour goes to the cell's albedo (u1) for the light pass.
const char* const kVoxMaterialHead = R"(
cbuffer VoxGeom : register(b13)
{
    float4 grid; // xyz the grid's first cell, w cells per axis
    float4 job;  // x the axis projected along, y the frame stamp, z the cell's opacity, w 1: alpha test
};
RWTexture3D<uint> occupancy : register(u0);
RWTexture3D<unorm float4> albedo : register(u1);
SamplerState linearWrap : register(s15);
)";
const char* const kVoxMaterialBody1 = R"(
void voxelize(In i)
{
    float4 pos = )";
const char* const kVoxMaterialBody2 = R"(;
    float4 c = material.Sample(linearWrap, )";
const char* const kVoxMaterialBody3 = R"();
    if (job.w > 0 && c.a < 0.5)
        return;
    int n = (int)grid.w;
    int3 uvw = int3((int)pos.x, (int)pos.y, (int)(pos.z * n));
    // The first and last slice along the projection are left out: vertex shaders that pin depth to a plane (far
    // geometry and sky at the far plane, shadow pancaking at the near) put whole faces there, a wall of cells at the
    // grid's edge (the user: a cube like a sky dome around the camera in the Num2 view, walls at the voxels' reach).
    if (any(uvw < 0) || any(uvw >= n) || uvw.z == 0 || uvw.z == n - 1)
        return;
    int axis = (int)job.x;
    int3 local = axis == 0 ? int3(uvw.z, uvw.x, uvw.y) : axis == 1 ? int3(uvw.x, uvw.z, uvw.y) : uvw;
    int3 t = ((int3)grid.xyz + local) % n;
    t = (t + n) % n;
    uint old;
    InterlockedMax(occupancy[t], (uint)job.y * 256 + (uint)(saturate(job.z) * 255), old);
    albedo[t] = float4(saturate(c.rgb), 1);
}
)";

#include "d3d12/resources.inc"
#include "d3d12/pipelines.inc"

constexpr int kQuerySlots = 4096;
constexpr int kFramesInFlight = 3;

class SwapChain12;

class Device final : public ID3D10Device1
{
public:
    explicit Device(UINT flags) : m_creationFlags(flags) {}

    bool Init();

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (IsEqualIID(riid, __uuidof(ID3D10Device)) || IsEqualIID(riid, __uuidof(ID3D10Device1)) || IsEqualIID(riid, __uuidof(IUnknown)))
        {
            AddRef();
            *out = static_cast<ID3D10Device1*>(this);
            return S_OK;
        }
        *out = nullptr;
        LogGuid("device", riid);
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_refs.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = m_refs.fetch_sub(1) - 1;
        if (refs == 0)
            Log("device released by the renderer (kept until exit)");
        return refs;
    }

    // ---- shader stage bindings
    void SetConstantBuffers(Stage s, UINT start, UINT n, ID3D10Buffer* const* buffers)
    {
        for (UINT i = 0; i < n && start + i < D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++)
            Bind(m_stage[s].cb[start + i], buffers ? static_cast<Buffer12*>(buffers[i]) : nullptr);
    }

    uint64_t m_envBinds = 0; // environment cube maps bound (per frame in the heap line)
    uint64_t m_env2DBinds = 0; // ... of them 2D maps (the weapons' Metal and every other material with $Env)
    Srv12* m_envTestCube = nullptr;
    Srv12* m_envTest2D = nullptr;
    bool m_envTestTried = false;

#include "d3d12/env.inc"
#include "d3d12/api.inc"
#include "d3d12/copies.inc"
#include "d3d12/gi_settings.inc"
#include "d3d12/targets.inc"
#include "d3d12/frame.inc"
#include "d3d12/gi_setup.inc"
#include "d3d12/sss.inc"
#include "d3d12/hiz.inc"
#include "d3d12/fog.inc"
#include "d3d12/cube.inc"
#include "d3d12/gi.inc"
#include "d3d12/vox.inc"
#include "d3d12/panel.inc"

    // At present: the voxel debug view (CRYMT_GI_VOXELVIEW or Num2) and the GI test keys' panel, drawn into the finished
    // frame before captures see it.
    void RunVoxelView(TextureCore* backbuffer)
    {
        const GiSettings& s = Gi();
        const DXGI_FORMAT f = VoxViewFormat(backbuffer->format);
        const bool panel = UpdatePanel(backbuffer->width, backbuffer->height) && m_panelState == kSrvState;
        const float mode = s.voxelView > 0 && m_voxValid ? s.voxelView : 0.0f;
        if ((mode <= 0 && !panel) || f == DXGI_FORMAT_UNKNOWN || !EnsureVox())
            return;
        if (!m_voxView || m_voxViewW != backbuffer->width || m_voxViewH != backbuffer->height || m_voxViewFormat != f)
        {
            DeferRelease(m_voxView);
            m_voxView = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = backbuffer->width;
            d.Height = backbuffer->height;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = f;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&m_voxView))))
                return;
            m_voxViewW = backbuffer->width;
            m_voxViewH = backbuffer->height;
            m_voxViewFormat = f;
        }
        Require(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FlushBarriers();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        VoxTable(cpu, gpu);
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        if (panel)
            g.dev->CreateShaderResourceView(m_panelTex, nullptr, at(0));
        else
            NullSrv(at(0));
        if (m_giFull)
            g.dev->CreateShaderResourceView(m_giFull, nullptr, at(1));
        else
            NullSrv(at(1));
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(backbuffer->res, &sd, at(2));
        VoxUav(at(3), 0, 0);
        g.dev->CreateUnorderedAccessView(m_voxView, nullptr, nullptr, at(4));
        VoxUav(at(5), -1);
        VoxUav(at(6), 0, 1);
        VoxUav(at(7), 0, 2);
        VoxUav(at(8), 0, 3);
        L1Uav(at(9), -1);
        L1Uav(at(10), -1);
        for (int f = 0; f < 12; f++)
            SixUav(at(11 + f), f % 6, -1);
        float toWorld[16];
        RigidInverse(m_giView, toWorld);
        float c[kVoxConstants] = {};
        VoxConstants(c, toWorld, backbuffer->width, backbuffer->height, mode);
        for (int l = 1; l < kVoxLevels; l++) // levels 1..3: their first cells in worldToPrevClip rows 1..3
            for (int i = 0; i < 3; i++)
                c[16 + l * 4 + i] = static_cast<float>(m_voxGrid[l][i]);
        if (panel) // left, centred in height
        {
            c[16] = static_cast<float>(backbuffer->height / 60 + 8);
            c[17] = static_cast<float>((static_cast<int>(backbuffer->height) - static_cast<int>(m_panelH)) / 2);
            c[18] = static_cast<float>(m_panelW);
            c[19] = static_cast<float>(m_panelH);
        }
        m_list->SetComputeRootSignature(m_voxRoot);
        m_list->SetComputeRoot32BitConstant(3, 0, 0);
        m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
        m_list->SetPipelineState(m_voxPsoView);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->Dispatch((backbuffer->width + 7) / 8, (backbuffer->height + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER toCopy = Transition(m_voxView, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->ResourceBarrier(1, &toCopy);
        Require(backbuffer, D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        m_list->CopyResource(backbuffer->res, m_voxView);
        m_barriers.push_back(Transition(m_voxView, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        RestoreListState();
    }

    void SceneDone(const DlssSceneInfo* info)
    {
        m_stats.scenes++;
        m_pastScene = true;
        m_doneColour = m_rtv[0] ? m_rtv[0]->info : nullptr;
        if (CopyLog())
            m_handOverPass = CurrentPass();
        if (info)
        {
            m_note.jx = info->jitterX;
            m_note.jy = info->jitterY;
        }
        GiSceneDone(info);
        GiCamera(info);
        m_giInScene = false; // until the next frame's camera
        // CRYMT_DLSS_NOEVAL: jitter alone, to check it; "after:N" from N seconds after start (a reference in the same run).
        static const DWORD noEvalTick = [] {
            wchar_t text[32] = {};
            if (!GetEnvironmentVariableW(L"CRYMT_DLSS_NOEVAL", text, 32))
                return DWORD(0);
            return wcsncmp(text, L"after:", 6) == 0 ? GetTickCount() + static_cast<DWORD>(_wtoi(text + 6)) * 1000 : DWORD(1);
        }();
        const bool noEval = noEvalTick && GetTickCount() >= noEvalTick;
        if (!dlss::Active() || !info || !info->valid || noEval)
            return;
        if (!m_rtv[0] || !m_dsv)
        {
            LogLimited("dlssbind", "scene: no render target or depth bound, DLSS skipped");
            return;
        }
        auto* color = static_cast<TextureCore*>(m_rtv[0]->info);
        auto* depth = static_cast<TextureCore*>(m_dsv->info);
        if (color->width != depth->width || color->height != depth->height || color->samples != 1)
        {
            LogLimited("dlsssize", "scene: color %ux%u and depth %ux%u differ, DLSS skipped", color->width, color->height, depth->width, depth->height);
            return;
        }
        if (!EnsureDlssTargets(color->width, color->height, color->format))
            return;
        const bool motion = RunMotion(*info, color, depth);
        const bool exposure = RunExposure(color, info->reset);
        const bool sanitized = RunSanitize(color);
        int dumpsLeft = g_sceneDumpFrames.load();
        while (dumpsLeft > 0 && !g_sceneDumpFrames.compare_exchange_weak(dumpsLeft, dumpsLeft - 1))
        {
        }
        const bool dumpScene = dumpsLeft > 0;
        if (dumpScene)
        {
            // One frame: next_dlss_in / next_mvec. A sequence (CRYMT_MVSEQ): mvNN_in, mvNN_mvec, mvNN_omb (the engine's motion
            // blur target, raw), mvNN_foliage, NN counting the frames of this capture.
            const bool seq = MvSequence();
            const int index = seq ? m_sceneDumpIndex++ : 0;
            m_seqDumpIndex = seq ? index : -1;
            char name[32];
            const auto named = [&](const char* what) {
                snprintf(name, sizeof(name), "mv%02d_%s", index, what);
                return name;
            };
            if (seq)
            {
                // <tag>_mvinfo.txt: per captured frame the jitter DLSS is told, the reset flag, the projection's offset
                // terms (0 without jitter) and the camera's place, for tools\prof\dlaa_seq.py.
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_mvinfo.txt", CryMTCapture("").c_str(), m_dumpTag.c_str());
                FILE* f = nullptr;
                if (fopen_s(&f, path, index == 0 ? "w" : "a") == 0 && f)
                {
                    float toWorld[16];
                    RigidInverse(info->view, toWorld);
                    fprintf(f, "%d jitter %.5f %.5f reset %d proj8 %.7f proj9 %.7f cam %.4f %.4f %.4f fwd %.5f %.5f %.5f time %.5f\n", index, info->jitterX,
                            info->jitterY, info->reset ? 1 : 0, info->proj[8], info->proj[9], toWorld[12], toWorld[13], toWorld[14], -toWorld[8], -toWorld[9],
                            -toWorld[10], info->time);
                    // The matrices themselves (tools: which view explains the picture's move).
                    const auto put = [&](const char* name, const float* m) {
                        fprintf(f, "%d %s", index, name);
                        for (int i = 0; i < 16; i++)
                            fprintf(f, " %.9g", m[i]);
                        fprintf(f, "\n");
                    };
                    put("view", info->view);
                    put("proj", info->proj);
                    put("viewLate", info->viewLate);
                    put("viewPrevLate", info->viewPrevLate);
                    put("viewStackLate", info->viewStackLate);
                    fprintf(f, "%d camPos %.6f %.6f %.6f %.1f\n", index, info->camPos[0], info->camPos[1], info->camPos[2], info->camPos[3]);
                    fclose(f);
                }
            }
            Require(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            RecordDump(color->res, seq ? named("in") : "next_dlss_in", 0);
            D3D12_RESOURCE_BARRIER b = Transition(m_dlssMvec, kSrvState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_list->ResourceBarrier(1, &b);
            RecordDump(m_dlssMvec, seq ? named("mvec") : "next_mvec", 3);
            m_barriers.push_back(Transition(m_dlssMvec, D3D12_RESOURCE_STATE_COPY_SOURCE, kSrvState));
            if (seq && MvSequenceLevel() != 2)
            {
                if (m_ombCopyFrame == m_frame && m_ombCopy.res)
                {
                    Require(&m_ombCopy, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    FlushBarriers();
                    RecordDump(m_ombCopy.res, named("omb"), 7);
                }
                if (m_objMvSrv && m_objMvFrame == m_frame && m_objMvSrv->info)
                {
                    Require(m_objMvSrv->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    FlushBarriers();
                    RecordDump(m_objMvSrv->info->res, named("objmv"), 7);
                }
                if (m_foliageSrv && m_foliageFrame == m_frame && m_foliageSrv->info)
                {
                    Require(m_foliageSrv->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    FlushBarriers();
                    RecordDump(m_foliageSrv->info->res, named("foliage"), 4);
                }
                if (m_biasSrv && m_biasFrame == m_frame && m_biasSrv->info)
                {
                    Require(m_biasSrv->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    FlushBarriers();
                    RecordDump(m_biasSrv->info->res, named("bias"), 9);
                }
                if (m_bendSrv && m_bendFrame == m_frame && m_bendSrv->info)
                {
                    Require(m_bendSrv->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    FlushBarriers();
                    RecordDump(m_bendSrv->info->res, named("bend"), 3);
                }
            }
        }
        FlushBarriers();
        // DLSS's bias mask, when water or particles wrote it this frame (BindBiasMask); CRYMT_DLSS_BIAS=0 keeps it from DLSS.
        static const bool biasOn = [] {
            wchar_t v[8] = {};
            return !(GetEnvironmentVariableW(L"CRYMT_DLSS_BIAS", v, 8) && _wtoi(v) == 0);
        }();
        Srv12* bias = biasOn && m_biasSrv && m_biasFrame == m_frame && m_biasW == color->width && m_biasH == color->height ? m_biasSrv : nullptr;
        if (bias)
        {
            Require(bias->info, kSrvState);
            FlushBarriers();
            m_stats.biasGiven++;
        }
        // The model comparison (panel: Engine > Compare DLAA models): a second DLSS with the other model into m_dlssOut2,
        // its right half put over the main output below.
        const bool compare = g_giControl.dlssCompare >= 0;
        if (compare && !m_dlssOut2)
        {
            D3D12_RESOURCE_DESC d = m_dlssOut->GetDesc();
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&m_dlssOut2))))
                m_dlssOut2 = nullptr;
        }
        else if (!compare && m_dlssOut2)
        {
            DeferRelease(m_dlssOut2);
            m_dlssOut2 = nullptr;
        }
        const dlss::Images images{sanitized ? m_dlssIn : color->res, sanitized ? kSrvState : color->state, depth->res, depth->state, m_dlssMvec, kSrvState,
                                  m_dlssOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, color->width, color->height, motion,
                                  exposure ? m_dlssExposure : nullptr, kSrvState, bias ? bias->info->res : nullptr, kSrvState,
                                  compare ? m_dlssOut2 : nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        PassName("CryMT DLAA");
        const bool done = dlss::Evaluate(m_list, images, *info);
        PassNameEnd();
        RestoreListState();
        if (exposure)
            m_barriers.push_back(Transition(m_dlssExposure, kSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        if (sanitized)
            m_barriers.push_back(Transition(m_dlssIn, kSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        if (!done)
            return;
        if (compare && m_dlssOut2)
        {
            // The right half from the comparison's DLSS; the halves meet at the screen's middle.
            D3D12_RESOURCE_BARRIER cb[2] = {Transition(m_dlssOut2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                            Transition(m_dlssOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
            m_list->ResourceBarrier(2, cb);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = m_dlssOut;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = m_dlssOut2;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const UINT half = color->width / 2;
            const D3D12_BOX box{half, 0, 0, color->width, color->height, 1};
            m_list->CopyTextureRegion(&dst, half, 0, 0, &src, &box);
            cb[0] = Transition(m_dlssOut2, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cb[1] = Transition(m_dlssOut, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_list->ResourceBarrier(2, cb);
            // A light 2-pixel line where the halves meet (the same scene on both sides hides the seam otherwise).
            D3D12_CPU_DESCRIPTOR_HANDLE rcpu;
            D3D12_GPU_DESCRIPTOR_HANDLE rgpu;
            if (m_srvRing.TryAlloc(1, rcpu, rgpu))
            {
                const D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvCpu.Alloc();
                g.dev->CreateUnorderedAccessView(m_dlssOut, nullptr, nullptr, cpu);
                g.dev->CreateUnorderedAccessView(m_dlssOut, nullptr, nullptr, rcpu);
                const FLOAT light[4] = {3.0f, 3.0f, 3.0f, 1.0f};
                const D3D12_RECT line{static_cast<LONG>(half) - 1, 0, static_cast<LONG>(half) + 1, static_cast<LONG>(color->height)};
                m_list->ClearUnorderedAccessViewFloat(rgpu, cpu, m_dlssOut, light, 1, &line);
                g_srvCpu.Free(cpu); // read when the clear was recorded
            }
            m_stats.dlssCompares++;
        }
        RunSanitizeOut(color->width, color->height);
        Require(color, D3D12_RESOURCE_STATE_COPY_DEST);
        m_barriers.push_back(Transition(m_dlssOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        FlushBarriers();
        m_list->CopyResource(color->res, m_dlssOut);
        m_note.dlss = 1;
        // The copies of the scene this frame made before DLSS (jittered) get DLSS's picture too (key "]"): the game's post
        // read them after DLSS (2 copies, 3 reads a frame in the user's shaking frames: the jitter was back in the picture,
        // the image moved by minus the jitter's change each frame; gone with this).
        if (g_giControl.fixCopies)
            for (uint32_t i = 0; i < m_preCopyCount; i++)
            {
                auto* t = const_cast<TextureCore*>(static_cast<const TextureCore*>(m_preCopies[i]));
                if (t == color || t->width != color->width || t->height != color->height || t->format != color->format || t->samples != 1)
                    continue;
                Require(t, D3D12_RESOURCE_STATE_COPY_DEST);
                FlushBarriers();
                m_list->CopyResource(t->res, m_dlssOut);
                m_note.fixed++;
            }
        if (dumpScene)
        {
            char name[32];
            snprintf(name, sizeof(name), "mv%02d_out", m_seqDumpIndex);
            RecordDump(m_dlssOut, m_seqDumpIndex >= 0 ? name : "next_dlss_out", 0);
        }
        m_barriers.push_back(Transition(m_dlssOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        m_stats.dlssFrames++;
    }
#include "d3d12/dumps.inc"
#include "d3d12/dlss.inc"

    Stats m_stats{};

private:
    struct StageState
    {
        Buffer12* cb[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        Srv12* srv[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        Sampler12* sampler[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        UINT srvEnd = 0; // highest slot ever set + 1, for hazard scans
        D3D12_GPU_VIRTUAL_ADDRESS cbApplied[8] = {};
        bool srvDirty = true, samplerDirty = true;
        UINT srvApplied = 0, samplerApplied = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE srvTable{}; // the table last set (the GI geometry pass reuses the PS's)
    };
    struct VertexBinding
    {
        Buffer12* buffer = nullptr;
        UINT stride = 0, offset = 0;
    };

    std::atomic<ULONG> m_refs{1};
    UINT m_creationFlags;
    UINT m_exceptionMode = 0;
    UINT m_textFilter[2] = {};

    StageState m_stage[kStages];
    VS12* m_vs = nullptr;
    GS12* m_gs = nullptr;
    PS12* m_ps = nullptr;
    Layout12* m_layout = nullptr;
    VertexBinding m_vb[16];
    D3D12_VERTEX_BUFFER_VIEW m_vbApplied[16] = {};
    Buffer12* m_ib = nullptr;
    DXGI_FORMAT m_ibFormat = DXGI_FORMAT_R16_UINT;
    UINT m_ibOffset = 0;
    D3D12_INDEX_BUFFER_VIEW m_ibApplied{};
    D3D10_PRIMITIVE_TOPOLOGY m_topology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Rtv12* m_rtv[8] = {};
    Dsv12* m_dsv = nullptr;
    Blend12* m_blend = nullptr;
    float m_blendFactor[4] = {1, 1, 1, 1};
    UINT m_sampleMask = 0xFFFFFFFF;
    Depth12* m_depth = nullptr;
    UINT m_stencilRef = 0;
    Raster12* m_raster = nullptr;
    D3D10_VIEWPORT m_vp10[16] = {};
    D3D12_VIEWPORT m_viewports[16] = {};
    UINT m_numViewports = 0;
    D3D10_RECT m_scissors[16] = {};
    UINT m_numScissors = 0;
    bool m_psoPending = false; // the last Pso() found its pipeline being created in the background
    bool m_psoDirty = true, m_psoValid = false, m_topologyDirty = true, m_omDirty = true, m_viewportDirty = true, m_scissorDirty = true;
    bool m_blendFactorDirty = true, m_stencilRefDirty = true;
    ID3D12PipelineState* m_psoApplied = nullptr;

    ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12CommandAllocator* m_alloc = nullptr;
    AllocatorPool m_allocs;
    std::vector<D3D12_RESOURCE_BARRIER> m_barriers;
    UploadRing m_frameRing;
    DescriptorRing m_srvRing;
    DescriptorRing m_samplerHeap; // used linearly; sampler tables are cached
    std::unordered_map<SamplerKey, D3D12_GPU_DESCRIPTOR_HANDLE, BytesHash> m_samplerTables;
    std::unordered_map<PsoKey, ID3D12PipelineState*, BytesHash> m_psos;
    ID3D12Resource* m_zeroCb = nullptr;
    Srv12* m_giSrv = nullptr;          // CRYMT_GITEST texture
    D3D12_CPU_DESCRIPTOR_HANDLE m_giCpu{}; // the view pixel shaders see in place of the placeholder (none: the placeholder)
    ID3D12Resource* m_fplusTex = nullptr;     // 1024x128 RGBA32F: the Forward+ test's lamps and tile lists (UpdateFPlus)
    D3D12_CPU_DESCRIPTOR_HANDLE m_fplusCpu{}; // its view, in place of the Forward+ placeholder
    std::vector<float> m_fplusData, m_fplusLast;
    std::vector<uint32_t> m_fplusCount, m_fplusStart;
    struct FPlusStats
    {
        uint32_t lamps = 0, onScreen = 0, litTiles = 0, maxPerTile = 0, indices = 0;
        uint64_t uploads = 0;
    } m_fplusStats;
    ID3D12Resource* m_jitterTex = nullptr;    // 8x1 RGBA32F: the frame texture for the shadow mask (UpdateFrameTexture)
    D3D12_CPU_DESCRIPTOR_HANDLE m_jitterCpu{}; // its view, in place of the jitter placeholder
    // The foliage mask (R8, the scene's size): the patched z-prepass's SV_Target1, 1 on alpha-tested surfaces; cleared
    // at the frame's first z-prepass draw (BindFoliageMask). Read by the contact shadows (in place of the foliage
    // placeholder) and GTAO.
    ID3D10Texture2D* m_foliageTex = nullptr;
    Rtv12* m_foliageRtv = nullptr;
    Rtv12* m_shadowMaskRtv = nullptr; // the deferred shadow pass's target (the sun's shadow in red), for GTAO
    uint64_t m_giRunFrame = ~0ull;      // m_frame of the last GI pass: the shaders get the GI texture up to 2 frames later
    // Per frame, for the capture key's log (the DLAA shake): DLSS and GI done, after the scene's hand-over the number of
    // reads of the DLSS'd colour, draws into it (with depth: 3D content after DLSS), the jitter DLSS was told.
    struct FrameNote
    {
        uint64_t frame;
        uint32_t dlss, gi, colourReads, lateDraws, late3D;
        uint32_t copies, copyReads; // copies of the scene colour made before DLSS (copy calls, full-size draws reading it), their reads after
        uint32_t fixed;             // of them given DLSS's picture (key "]")
        float jx, jy;
    };
    const ResourceInfo* m_preCopies[8] = {};
    uint32_t m_preCopyCount = 0;
    // CRYMT_COPYLOG=1 with CRYMT_MARKERS=1 (diagnosis, 2026-11-13: which of the engine's passes copy the jittered scene
    // before DLSS and which read the copies after it - the "]" fix's clean form): per copy the pass and how it was made,
    // per read after the hand-over the pass and the pixel shader; logged every 600 frames.
    std::string m_preCopyWhere[8];
    std::string m_handOverPass;
    std::string m_preCopyReads[8];
    std::string CurrentPass() const
    {
        return m_passNames.empty() ? std::string("(no pass)") : std::string(m_passNames.back().begin(), m_passNames.back().end());
    }
    static bool CopyLog()
    {
        static const bool on = GetEnvironmentVariableW(L"CRYMT_COPYLOG", nullptr, 0) != 0;
        return on;
    }
    // A texture of the scene colour's size and format (the last hand-over's).
    bool LikeSceneColour(const ResourceInfo* info) const
    {
        if (!info || !m_doneColour || info->buffer || m_doneColour->buffer)
            return false;
        const auto* a = static_cast<const TextureCore*>(info);
        const auto* b = static_cast<const TextureCore*>(m_doneColour);
        return a->width == b->width && a->height == b->height && a->format == b->format;
    }
    void NotePreCopy(const ResourceInfo* dst, const char* how = "copy")
    {
        for (uint32_t i = 0; i < m_preCopyCount; i++)
            if (m_preCopies[i] == dst)
                return;
        if (m_preCopyCount < 8)
        {
            if (CopyLog())
            {
                char text[160];
                snprintf(text, sizeof(text), "%s in [%s] ps %016llx", how, CurrentPass().c_str(),
                         static_cast<unsigned long long>(m_ps ? m_ps->contentHash : 0));
                m_preCopyWhere[m_preCopyCount] = text;
                m_preCopyReads[m_preCopyCount].clear();
            }
            m_preCopies[m_preCopyCount++] = dst;
        }
        m_note.copies++;
    }
    FrameNote m_note{};
    FrameNote m_notes[16]{};
    const ResourceInfo* m_doneColour = nullptr;
    bool m_pastScene = false;
    Srv12* m_foliageSrv = nullptr;
    UINT m_foliageW = 0, m_foliageH = 0;
    uint64_t m_foliageFrame = ~0ull;
    ID3D10Texture2D* m_biasTex = nullptr; // BindBiasMask
    Rtv12* m_biasRtv = nullptr;
    Srv12* m_biasSrv = nullptr;
    UINT m_biasW = 0, m_biasH = 0;
    uint64_t m_biasFrame = ~0ull;
    ID3D10Texture2D* m_partTex[2] = {}; // BindParticleLayer
    Rtv12* m_partRtv[2] = {};
    Srv12* m_partSrv[2] = {};
    UINT m_partW = 0, m_partH = 0;
    uint64_t m_partFrame[2] = {~0ull, ~0ull};
    uint64_t m_partDraws = 0;
    ID3D10Texture2D* m_bendTex = nullptr; // BindBendVelocity
    Rtv12* m_bendRtv = nullptr;
    Srv12* m_bendSrv = nullptr;
    UINT m_bendW = 0, m_bendH = 0;
    uint64_t m_bendFrame = ~0ull;
    int m_seqDumpIndex = -1;               // the CRYMT_MVSEQ frame being dumped (-1: a single capture-key frame)
    ID3D10Texture2D* m_objMvTex = nullptr; // BindObjectMotion
    Rtv12* m_objMvRtv = nullptr;
    Srv12* m_objMvSrv = nullptr;
    UINT m_objMvW = 0, m_objMvH = 0;
    uint64_t m_objMvFrame = ~0ull;
    float m_mvPrevTime = 0;
    bool m_mvHavePrevTime = false;
    float m_frameLast[64] = {1e9f};             // the frame texture's last data
    // The water's SSR on the previous frame's scene (WaterPrevFrame): the GI's colour copy and its frame, its view, the
    // previous main camera (view, projection, place, jitter) and its frame.
    uint64_t m_giColorFrame = ~0ull, m_ssrPrevCamFrame = ~0ull;
    bool m_ssrPrevOn = false;
    uint64_t m_ssrPrevBinds = 0;
    ID3D12Resource* m_ssrPrevRes = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_ssrPrevSrv{};
    float m_ssrPrevView[16] = {}, m_ssrPrevProj[16] = {}, m_ssrPrevCamPos[4] = {}, m_ssrPrevShift[2] = {};
    float m_ssrCurView[16] = {}, m_ssrCurProj[16] = {}, m_ssrCurCamPos[4] = {}, m_ssrCurShift[2] = {};
    uint64_t m_ssrCurFrame = ~0ull;
    ID3D12Resource* m_giColor = nullptr;   // previous frame's HDR scene colour
    ID3D12Resource* m_giHist[2] = {};      // half-resolution GI, ping-pong
    D3D12_CPU_DESCRIPTOR_HANDLE m_giHistSrv[2] = {};
    D3D12_RESOURCE_STATES m_giHistState[2] = {};
    ID3D12RootSignature* m_giRoot = nullptr;
    ID3D12RootSignature* m_depthAlignRoot = nullptr; // SceneDepth (the editor's viewports)
    ID3D12PipelineState* m_depthAlignPso = nullptr;
    bool m_depthAlignFailed = false;
    TextureCore m_depthAligned{};
    float m_depthAlignRect[4] = {};
    // Bend's screen-space shadows (RunSss): R8 occlusion at the z-prepass target's size (unordered access, then read by
    // the deferred shadow pass in place of the sss placeholder) and its view; the pipeline; the frames they were tried and
    // made in; the sun's clip position with the frame's jitter and the far plane (UpdateFrameTexture); the z-prepass
    // target the frame's z-prepass draws wrote (BindFoliageMask).
    TextureCore m_sss{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_sssSrv{};
    ID3D12RootSignature* m_sssRoot = nullptr;
    ID3D12PipelineState* m_sssPso = nullptr;
    bool m_sssFailed = false;
    uint64_t m_sssTriedFrame = ~0ull, m_sssFrame = ~0ull;
    float m_sssClip[4] = {};
    float m_sssFar = 0;
    bool m_sssSun = false;
    TextureCore* m_zTarget = nullptr;
    uint64_t m_zTargetFrame = ~0ull;
    UINT m_zMaxW = 0, m_zMaxH = 0, m_zPrevW = 0, m_zPrevH = 0; // BindFoliageMask: this frame's largest z-prepass, the last one's
    uint64_t m_zMaxFrame = ~0ull;
    // The depth pyramid for the water's SSR (RunHiz): the texture with all levels, its view, a UAV per level, the
    // pipelines, the frames it was tried and made in, near / far from the frame's camera.
    static constexpr UINT kHizLevels = 7;
    TextureCore m_hiz{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_hizSrv{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_hizUav[kHizLevels] = {};
    ID3D12RootSignature* m_hizRoot = nullptr;
    ID3D12PipelineState* m_hizInitPso = nullptr;
    ID3D12PipelineState* m_hizDownPso = nullptr;
    bool m_hizFailed = false;
    uint64_t m_hizTriedFrame = ~0ull, m_hizFrame = ~0ull;
    float m_hizNearOverFar = 0;
    // Volumetric fog, stage 1 (RunVfog): the volume over the screen (RGBA16F, 16-pixel tiles x kVfogSlices), its views,
    // the pipeline, the frames it was tried and made in, the scene target it was made for (only its fog pass reads it:
    // reflections and cube faces keep the engine's fog), the last fog constants read (vfParams, vfFogColor, Mie, Rayleigh).
    static constexpr UINT kVfogSlices = 64;
    static constexpr float kVfogReach = 128.0f; // metres; the patched FogPass has the same (CryMT_VFog). 64 m held only
                                                // ~40% of island's fog (2026-11-14), the sun's cascades reach ~160 m
    TextureCore m_vfog{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_vfogSrv{}, m_vfogUav{};
    TextureCore m_vfogParam{}; // the fog pass's parameters (1x1 RGBA32F: the near air, the mark 4242), written by the pass
    D3D12_CPU_DESCRIPTOR_HANDLE m_vfogParamSrv{}, m_vfogParamUav{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_vfogNull3d{}; // a null 3D view: the volume's slot when there is none (reads zero)
    ID3D12RootSignature* m_vfogRoot = nullptr;
    ID3D12PipelineState* m_vfogPso = nullptr;
    bool m_vfogFailed = false;
    uint64_t m_vfogTriedFrame = ~0ull, m_vfogFrame = ~0ull;
    const ResourceInfo* m_vfogTarget = nullptr;
    float m_vfogConstants[4][4] = {};
    // Stage 2, the sun's shadows in the fog: the sun's shadow maps seen drawn (NoteSunMap), kept by map so the cached
    // cascades (e_gsm_cache: not redrawn every frame) stay known; cleared with a reset (level load, camera cut). Per map
    // its light matrix (world positions, row vectors), the light's far distance, the frame it was last drawn in. A map
    // reused for another light leaves the table.
    struct SunMap
    {
        ResourceInfo* map = nullptr;
        float mv[16] = {};
        float lightFar = 0;
        uint64_t frame = 0;
    };
    static constexpr int kSunMaps = 6, kVfogCascades = 4;
    SunMap m_sunMaps[kSunMaps];
    bool m_vfogShadowPending = false, m_vfogPendingSun = false; // PassMatrices: a shadow map's matrix, the sun's
    float m_vfogPendingMv[16] = {};
    uint32_t m_vfogCascadesUsed = 0; // the last fog pass's cascades (for the log)
    ID3D12PipelineState* m_giPso = nullptr;       // trace
    ID3D12PipelineState* m_giPsoFilter = nullptr; // blur + temporal blend
    ID3D12Resource* m_giRaw = nullptr;            // traced result before the filter (unordered access between passes)
    ID3D12PipelineState* m_giPsoUpsample = nullptr; // full-size GI by distance
    ID3D12PipelineState* m_giPsoCamera = nullptr;   // the first-person view's GI value
    ID3D12Resource* m_giCam = nullptr;              // its 1x1 texture
    ID3D12PipelineState* m_giPsoProbes = nullptr;   // GI v2: the probes' rays and atlas (kGiShader "probes")
    ID3D12Resource* m_probeAtlas[2] = {};           // their atlases, written in turn (1024 x kProbeCascades by 1152 RGBA16F: tiles, states)
    ID3D12Resource* m_probeMatch = nullptr;         // GI v2: the cones' light matched to the probes' (kGiShader probeMatch, 16 uints)
    ID3D12Resource* m_probeDepth[2] = {};           // their distances (1024 x kProbeCascades by 1024 RG16F, tiles as the light's), with the atlas of the same index
    D3D12_RESOURCE_STATES m_probeAtlasState[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    int m_probeCur = 0;                             // the one written next
    bool m_probeFailed = false;
    ID3D12Resource* m_vox[kVoxLevels] = {};       // voxel grid levels (kVoxShader), unordered access but in the GI passes
    ID3D12Resource* m_voxDir[kVoxLevels] = {};    // their opacity per axis, RGBA8 at grid mips 1..4 (CRYMT_GI_MIPOPACITY=4)
    ID3D12Resource* m_voxL1[kVoxLevels] = {};     // their light's direction, RGBA8 SNORM with the grid's mips (CRYMT_GI_DIRLIGHT 1)
    ID3D12Resource* m_voxSix[kVoxLevels][6] = {}; // their light per direction (CRYMT_GI_DIRLIGHT 2, EnsureVoxSix)
    bool m_voxSixReady = false, m_voxSixFailed = false;
    ID3D12Resource* m_voxView = nullptr;          // its debug view, the back buffer's size and format
    UINT m_voxViewW = 0, m_voxViewH = 0;
    DXGI_FORMAT m_voxViewFormat = DXGI_FORMAT_UNKNOWN;
    ID3D12RootSignature* m_voxRoot = nullptr;
    ID3D12PipelineState* m_voxPsoUpdate = nullptr;
    ID3D12PipelineState* m_voxPsoView = nullptr;
    ID3D12PipelineState* m_voxPsoDown = nullptr;
    ID3D12PipelineState* m_voxPsoLight = nullptr; // sunlight for geometry cells (kVoxShader "light")
    ID3D12PipelineState* m_voxPsoRoof = nullptr;  // cover above (kVoxShader "roof")
    ID3D12PipelineState* m_voxPsoInject = nullptr; // the engine cube's face into the levels (kVoxShader "inject")
    ID3D12Resource* m_voxRoof = nullptr;          // its texture, R8 at half level 0's resolution
    float m_giSun[8] = {};                        // the last scene's sun (DlssSceneInfo::sun): towards it, valid, colour
    float m_giLamps[8][12] = {};                  // and its lamps (DlssSceneInfo::lamps)
    uint32_t m_giLampCount = 0;
    float m_giVisAreas[16][40] = {};              // and the VisAreas near the camera (DlssSceneInfo::visAreas)
    uint32_t m_giVisAreaCount = 0;
    ID3D12Resource* m_panelTex = nullptr; // GI test keys' panel (UpdatePanel)
    UINT m_panelW = 0, m_panelH = 0;
    D3D12_RESOURCE_STATES m_panelState = D3D12_RESOURCE_STATE_COPY_DEST;
    std::wstring m_panelText;
    bool m_voxFailed = false, m_voxValid = false;
    int m_voxGrid[kVoxLevels][3] = {}, m_voxPrevGrid[kVoxLevels][3] = {}; // per level: first cell now, at its last update
    // GI geometry (RunVoxelGeometry): this frame's z-prepass draws, where each vertex shader reads the camera matrices,
    // the camera buffers' contents, the occupancy grid (R32_UINT frame stamps, unordered access), the pass's signature,
    // pixel shader and pipelines (by vertex shader, layout and topology type).
    std::vector<VoxDraw> m_voxDraws;
    std::unordered_map<uint64_t, std::vector<VoxCameraPatch>> m_voxCamera;  // by vertex shader content hash
    std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, VoxCameraCopy> m_voxCameraCopies; // this frame's, by address
    // The same for the shadow map draws (PassMatrices: the frame's job), their own matrices by vertex shader.
    std::vector<VoxDraw> m_shadowDraws;
    std::unordered_map<uint64_t, std::vector<VoxCameraPatch>> m_shadowCamera;
    std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, VoxCameraCopy> m_shadowCopies;
    // The same for the engine cube's face draws (CubeRecording; the face camera from the renderer's matrices): solid
    // ones once per key, stamped as the cube's (RunVoxelGeometry) into the cells the camera does not see.
    std::vector<VoxDraw> m_cubeDraws;
    std::unordered_map<uint64_t, std::vector<VoxCameraPatch>> m_cubeCamera;
    std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, VoxCameraCopy> m_cubeCopies;
    std::unordered_set<uint64_t> m_cubeKeys;
    uint64_t m_cubeDrawsFrame = ~0ull; // the frame (g_presented) whose face draws m_cubeDraws holds
    uint64_t m_cubeTurn = 0;           // cube jobs run (their axis turns)
    float m_cubeToWorld[16] = {};      // the face camera's inverse view at its first recorded draw
    // The face being drawn, for the inject pass (CRYMT_GI_CUBEINJECT): its depth and HDR colour targets and its camera.
    bool m_cubeFaceNoted = false;
    ResourceInfo* m_cubeFaceDepth = nullptr;
    ResourceInfo* m_cubeFaceColour = nullptr;
    DXGI_FORMAT m_cubeFaceDepthFormat = DXGI_FORMAT_UNKNOWN; // its depth view's (D24_UNORM_S8_UINT...)
    float m_cubeFaceVp[16] = {}, m_cubeFaceProj[16] = {};
    float m_passMv[16] = {}, m_passProj[16] = {}; // the renderer's matrices now (stream order, kOpMatrices)
    float m_shadowMv[16] = {};                    // the light matrix of the current shadow map
    bool m_shadowRecording = false;               // the current shadow map's draws are this frame's job
    int m_shadowCurrent = -1;                     // the map being drawn (stats), -1 none
    uint64_t m_shadowFrame = ~0ull;               // m_giFrame of the maps counted below
    int m_shadowMaps = 0, m_shadowLastMaps = 0, m_shadowPick = -1, m_shadowLevel = 0, m_shadowAxis = 0;
    float m_shadowCover[8] = {}, m_shadowLastCover[8] = {};
    ID3D12Resource* m_voxOcc[kVoxGeomLevels] = {};
    ID3D12Resource* m_voxSeen[kVoxGeomLevels] = {}; // R8_UINT: the screen coloured the cell (the light pass skips it)
    ID3D12Resource* m_voxAlbedo[kVoxGeomLevels] = {}; // RGBA8: the material colour the geometry pass sampled (a 1 when set)
    std::unordered_map<uint64_t, ID3DBlob*> m_voxMaterialPs; // by vertex shader outputs and texture slot (null: failed)
    void* m_voxCompile = nullptr; // D3DCompile
    ID3D12RootSignature* m_voxGeomRoot = nullptr;
    ID3DBlob* m_voxGeomPs = nullptr;
    std::unordered_map<uint64_t, ID3D12PipelineState*> m_voxGeomPsos;
    bool m_voxGeomFailed = false;
    ID3D12Resource* m_giFull = nullptr;           // full-size GI the engine's shaders read
    D3D12_CPU_DESCRIPTOR_HANDLE m_giFullSrv{};
    DXGI_FORMAT m_giColorFormat = DXGI_FORMAT_UNKNOWN;
    UINT m_giW = 0, m_giH = 0, m_giFullW = 0, m_giFullH = 0;
    int m_giCur = 0;
    float m_giView[16] = {}, m_giProj[16] = {}; // the frame's main camera (GiCamera)
    float m_giCamPos[4] = {}, m_giPrevCamPos[4] = {}; // its place (DlssSceneInfo::camPos), and the previous GI pass's
    float m_giPrevView[16] = {}, m_giPrevProj[16] = {}, m_giPrevToWorld[16] = {}; // the last GI pass's camera
    bool m_giHaveCamera = false;
    // The object motion blur pass's target this frame (r_MotionBlur 3, Very High): RGBA16F, full size, drawn before
    // the scene is done: (now - before) * a distance fade in screen units, distance / far, object id.
    TextureCore* m_ombTarget = nullptr;
    TextureCore m_ombCopy{};          // KeepObjectMotion: the motion blur target right after its pass
    uint64_t m_ombCopyFrame = ~0ull;
    bool m_ombPending = false;
    struct OmbDiag
    {
        TextureCore* target;
        uint64_t vs, ps;
    };
    OmbDiag m_ombDiag[8] = {};
    uint32_t m_ombDiagCount = 0, m_ombDiagLogged = 0;
    uint64_t m_ombDiagFrame = ~0ull;
    uint64_t m_ombFrame = ~0ull;
    float m_mvPrevView[16] = {}, m_mvPrevProj[16] = {}, m_mvPrevToWorld[16] = {}; // the last motion pass's camera
    float m_mvPrevCamPos[4] = {};                                                  // its place (DlssSceneInfo::camPos)
    bool m_mvHavePrev = false;
    ID3D12Resource* m_giDist[2] = {};
    ID3D12PipelineState* m_mvPso = nullptr;
    ID3D12PipelineState* m_expPso = nullptr;
    ID3D12PipelineState* m_sanPso = nullptr;
    ID3D12PipelineState* m_sanOutPso = nullptr;
    ID3D12PipelineState* m_aoPso = nullptr;      // GTAO passes (gtao, gtaoBlur, gtaoApply)
    ID3D12PipelineState* m_aoBlurPso = nullptr;
    ID3D12PipelineState* m_aoApplyPso = nullptr;
    ID3D12PipelineState* m_pvolPso = nullptr; // the particles' occlusion volume (pvolume)
    ID3D12PipelineState* m_envPso = nullptr;  // environment maps from the voxel grid (envmap)
    ID3D12Resource* m_envAtlas[2] = {};       // its output (13 squares of kEnvSize) and the previous run's: the one written
    int m_envCur = 0;                         // next is UNORDERED_ACCESS, the other NON_PIXEL_SHADER_RESOURCE
    Srv12* m_envCube = nullptr;               // the cube and the 2D map the materials get (EnvSubstitute)
    Srv12* m_env2D = nullptr;
    Srv12* m_envCubeAll = nullptr;            // the cube for fragLib's reflection on every material
    Srv12* m_engineCube = nullptr;            // the engine-captured cube (EngineCube), RGBK
    ID3D10Resource* m_engineCubeRes = nullptr; // its texture (the view keeps it)
    uint64_t m_engineCubeViews = 0;
    // The engine's cube kept in HDR with its mips (RunCubeUpdate, kCubeShader): what the water reads once all six faces
    // came; each face is decoded, turned to D3D's layout and mixed in when the engine has drawn it.
    TextureCore m_hdrCube{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_hdrCubeSrv{}, m_hdrCubeUav[12]{};
    ID3D12RootSignature* m_cubeRoot = nullptr;
    ID3D12PipelineState* m_cubeDecodePso = nullptr;
    ID3D12PipelineState* m_cubeDownPso = nullptr;
    ID3D12PipelineState* m_cubeEncodePso = nullptr;
    // The materials' copies of it (cubePrefilter: GGX by level, encoded as the voxel cube, 1 - exp(-0.5 x)): every
    // material's (fragLib's CryMT_EnvSampler; the panel's "every material" scale) and their own maps' (its "own map" scale).
    TextureCore m_ldrCube{}, m_ldrCubeOwn{};
    // The 2D environment map from the engine's cube (RunEnv2D, for the first-person weapons' Metal), 64x64 RGBA16F, its
    // views, the pipeline; ready once made from the materials' cube.
    TextureCore m_ldr2D{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_ldr2DSrv{}, m_ldr2DUav{};
    ID3D12RootSignature* m_env2DRoot = nullptr;
    ID3D12PipelineState* m_env2DPso = nullptr;
    bool m_env2DFailed = false, m_ldr2DReady = false;
    D3D12_CPU_DESCRIPTOR_HANDLE m_ldrCubeSrv{}, m_ldrCubeUav[12]{}, m_ldrCubeOwnSrv{}, m_ldrCubeOwnUav[12]{};
    bool m_ldrCubeReady = false;
    bool m_cubeFailed = false;
    bool m_hdrFaceValid[6] = {};
    int m_engineCubeLastFace = -1; // the face drawn in this cube render (AfterDraw)
    bool m_engineCubeFresh = false; // this render's face replaces the old one (after a jump, a change of the time of day)
    uint64_t m_hdrCubeRuns = 0;
    bool m_engineCubeDumpWanted = false;
    D3D12_CPU_DESCRIPTOR_HANDLE m_trilinearClamp{}; // the water's sampler for CryMT's cube
    bool m_engineCubePass = false; // between CryMT's "CryMT engine cube" marks (PassName), stream order
    int m_engineCubeFace = -1;     // the engine's face being drawn (the start mark's " f<n>"), -1 not told
    bool m_envReady = false, m_envFailed = false;
    ID3D12Resource* m_pvol = nullptr;         // its atlas: 1024 x 513 RGBA32F (fine, coarse; shares, place row, tints), pvol.dds
    D3D12_CPU_DESCRIPTOR_HANDLE m_pvolCpu{};  // its view for the pixel shaders
    bool m_pvolBuilt = false;                 // written by a GI pass (else the placeholder: no change)
    uint64_t m_pvolFrame = ~0ull;             // the frame (m_frame) it was last written in
    ID3D12Resource* m_aoRaw = nullptr;  // half-size occlusion, raw (R16F, unordered access between frames)
    ID3D12Resource* m_aoBlur = nullptr; // (unused since the history)
    ID3D12Resource* m_aoHist[2] = {};   // half-size occlusion accumulated (R16G16F: occlusion, distance), ping-pong
    D3D12_RESOURCE_STATES m_aoHistState[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    int m_aoCur = 0;
    ID3D12Resource* m_dlssIn = nullptr; // CRYMT_DLSS_CLAMP: the sanitized colour (unordered access between frames)
    ID3D12Resource* m_dlssExposure = nullptr; // 1x1 R32F, unordered access between frames
    uint64_t m_giFrame = 0, m_giLastRun = 0;
    bool m_giInScene = false; // between the frame's camera (kOpCamera) and SceneDone
    const ResourceInfo* m_giSceneDepth = nullptr; // the depth buffer bound at the last SceneDone (the scene's)
    const ResourceInfo* m_giSceneColor = nullptr; // and the colour target (a reflection may share the depth buffer)
    const ResourceInfo* m_giSceneColorPrev = nullptr; // the one before it, when the scene's target changed
    // One of the scene's own colour targets: the engine may draw the scene into either of two HDR targets by turns (seen
    // with r_MotionBlur 0: the GI ran every second frame, the others showed the GI of two frames before as a ghost). Any
    // target of the scene's size and format let the GI run in other passes too (objects flickered frame to frame).
    bool SceneColourLike(const ResourceInfo* info) const
    {
        return info && (info == m_giSceneColor || info == m_giSceneColorPrev);
    }
    bool m_giReset = true, m_giFailed = false;
    UINT m_drawsSinceSubmit = 0;

    ID3D12QueryHeap* m_occlusionHeap = nullptr;
    ID3D12QueryHeap* m_timestampHeap = nullptr;
    ID3D12Resource* m_queryReadback = nullptr;
    const uint64_t* m_queryCpu = nullptr;
    std::vector<int> m_freeOcclusion, m_freeTimestamp;
    std::mutex m_queryLock;

    uint64_t m_frameFences[kFramesInFlight] = {};
    uint64_t m_frame = 0;
    bool m_removedLogged = false;
    bool m_passNamesOn = rdoc::Requested() || GetEnvironmentVariableW(L"CRYMT_MARKERS", nullptr, 0) != 0;
    std::vector<std::wstring> m_passNames;
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullRtv{}; // for gaps between bound render targets
    size_t m_activeQueries = 0;              // occlusion queries begun and not ended
    ID3D12Resource* m_dlssOut = nullptr;
    ID3D12Resource* m_dlssOut2 = nullptr; // the model comparison's second DLSS output (GiControl::dlssCompare)
    ID3D12Resource* m_dlssMvec = nullptr;
    int m_sceneDumpIndex = 0; // the frame number of a motion vector sequence dump (CRYMT_MVSEQ)
    UINT m_dlssWidth = 0, m_dlssHeight = 0;
    DXGI_FORMAT m_dlssFormat = DXGI_FORMAT_UNKNOWN;
    ID3D12QueryHeap* m_listStampHeap = nullptr;
    // GI pass timers: kGiStamps timestamps a GI run (start, trace, filter, upsample, voxel geometry, voxel update), in a
    // ring of kGiStampRuns runs; a slot is read back when it comes round again (long done).
    static constexpr UINT kGiStamps = 6, kGiStampRuns = 16;
    ID3D12QueryHeap* m_giStampHeap = nullptr;
    ID3D12Resource* m_giStampReadback = nullptr;
    const uint64_t* m_giStamps = nullptr;
    uint64_t m_giStampRun = 0;
    void GiStamp(UINT i)
    {
        if (m_giStamps)
            m_list->EndQuery(m_giStampHeap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(m_giStampRun % kGiStampRuns) * kGiStamps + i);
    }
    ID3D12Resource* m_listStampReadback = nullptr;
    const uint64_t* m_listStamps = nullptr;
    uint64_t m_stampedSerial = 0; // main lists up to this one are counted in gpuTicks

public:
    // GPU time per pass (CRYMT_GPUPASSES=1 with CRYMT_MARKERS): a timestamp at the start and end of every main list and at
    // every pass name boundary; each span counts for the innermost open name, or "after <last closed name>" between
    // names, so the spans add up to the GPU busy time.
    static constexpr UINT kPassStamps = 2048; // per list
    static constexpr UINT kPassRing = 8;      // lists that may be in flight
    bool m_passStampsOn = false;
    ID3D12QueryHeap* m_passStampHeap = nullptr;
    ID3D12Resource* m_passStampReadback = nullptr;
    const uint64_t* m_passStampCpu = nullptr;
    struct PassStampList
    {
        uint64_t serial = 0;
        std::vector<uint16_t> labels;
    };
    PassStampList m_passLists[kPassRing];
    std::vector<uint16_t> m_passLabels; // the list being recorded
    std::vector<std::string> m_passLabelNames;
    std::unordered_map<std::string, uint16_t> m_passLabelIds;
    std::vector<uint64_t> m_passTicks;
    std::string m_passLastEnded;
    uint64_t m_passStampedSerial = 0;
    uint64_t m_passStampsDropped = 0;

    std::string PassLabel() const
    {
        if (m_passNames.empty())
            return "after " + (m_passLastEnded.empty() ? std::string("list start") : m_passLastEnded);
        const std::wstring& w = m_passNames.back();
        return std::string(w.begin(), w.end());
    }

    void PassStamp(const std::string& label)
    {
        if (!m_passStampsOn)
            return;
        if (m_passLabels.size() >= kPassStamps)
        {
            m_passStampsDropped++;
            return;
        }
        auto it = m_passLabelIds.find(label);
        if (it == m_passLabelIds.end())
        {
            if (m_passLabelNames.size() >= 65535)
                return;
            it = m_passLabelIds.emplace(label, static_cast<uint16_t>(m_passLabelNames.size())).first;
            m_passLabelNames.push_back(label);
            m_passTicks.push_back(0);
        }
        const UINT index = static_cast<UINT>(g_mainSerial % kPassRing) * kPassStamps + static_cast<UINT>(m_passLabels.size());
        m_list->EndQuery(m_passStampHeap, D3D12_QUERY_TYPE_TIMESTAMP, index);
        m_passLabels.push_back(it->second);
    }

    void PassStampsSubmit() // before Close
    {
        if (!m_passStampsOn)
            return;
        PassStamp("(list end)");
        const UINT base = static_cast<UINT>(g_mainSerial % kPassRing) * kPassStamps;
        const UINT count = static_cast<UINT>(m_passLabels.size());
        if (count)
            m_list->ResolveQueryData(m_passStampHeap, D3D12_QUERY_TYPE_TIMESTAMP, base, count, m_passStampReadback, base * sizeof(uint64_t));
        PassStampList& l = m_passLists[g_mainSerial % kPassRing];
        l.serial = g_mainSerial;
        l.labels.swap(m_passLabels);
        m_passLabels.clear();
    }

    void PassStampsRead() // after Retire
    {
        if (!m_passStampsOn)
            return;
        const uint64_t done = g_doneSerial.load(std::memory_order_acquire);
        for (; m_passStampedSerial < done; m_passStampedSerial++)
        {
            const uint64_t serial = m_passStampedSerial + 1;
            const PassStampList& l = m_passLists[serial % kPassRing];
            if (l.serial != serial || l.labels.size() < 2)
                continue;
            const uint64_t* s = m_passStampCpu + (serial % kPassRing) * kPassStamps;
            for (size_t i = 0; i + 1 < l.labels.size(); i++)
                if (s[i + 1] > s[i])
                    m_passTicks[l.labels[i]] += s[i + 1] - s[i];
        }
    }

private:

    void SplitQueries(bool end);
};

#include "d3d12/device.inc"
#include "d3d12/create.inc"
#include "d3d12/queries.inc"
#include "d3d12/present.inc"
HRESULT Dx12QueryPoll(ID3D10Asynchronous* query, void* data, UINT size, UINT flags, uint64_t* flushSerial)
{
    *flushSerial = 0;
    return dx12::g_device ? dx12::g_device->QueryPeek(static_cast<dx12::Query12*>(static_cast<ID3D10Query*>(query)), data, size, flags, flushSerial) : S_FALSE;
}

void Dx12QueryFlush(uint64_t serial)
{
    if (dx12::g_device && serial >= dx12::g_mainSerial)
        dx12::g_device->Submit();
}

bool Dx12DiscardNow(ID3D10Buffer* buffer, Dx12Discard* out)
{
    return dx12::DiscardNow(buffer, out);
}

void* Dx12ProducerMemory(ID3D10Buffer* buffer)
{
    return dx12::ProducerMemory(buffer);
}

void Dx12ApplyDiscard(ID3D10Buffer* buffer, const uint64_t* words)
{
    dx12::ApplyDiscard(buffer, words);
}

void Dx12DirectMapStats(uint64_t* discards, uint64_t* bytes)
{
    dx12::DirectMapStats(discards, bytes);
}

ID3D10Device* Dx12CreateDevice(IDXGIAdapter* adapter, UINT flags)
{
    using namespace dx12;
    if (!InitCore(adapter))
        return nullptr;
    auto* device = new Device(flags);
    g_device = device;
    if (!device->Init())
    {
        Log("device init failed");
        g_device = nullptr;
        return nullptr;
    }
    return device;
}

void Dx12SceneDone(const DlssSceneInfo* info)
{
    if (dx12::g_device)
        dx12::g_device->SceneDone(info);
}

void Dx12Camera(const DlssSceneInfo* info)
{
    if (dx12::g_device)
        dx12::g_device->GiFrameCamera(info);
}

void Dx12PanelKey(int action)
{
    dx12::Device::PanelKey(action);
}

void Dx12PassMatrices(const float* matrices)
{
    if (dx12::g_device)
        dx12::g_device->PassMatrices(matrices);
}

void Dx12PassName(const char* text)
{
    if (!dx12::g_device)
        return;
    if (text && *text)
        dx12::g_device->PassName(text);
    else
        dx12::g_device->PassNameEnd();
}

bool Dx12WantsPassMatrices()
{
    return dx12::g_wantPassMatrices.load(std::memory_order_relaxed);
}

void Dx12SetObjectMotion(bool on)
{
    dx12::g_objectMotion = on;
}

void Dx12CaptureNow(int frames, int flags)
{
    dx12::g_captureKeyFlags.store(flags);
    dx12::g_captureKeyFrames.store(frames);
}

void Dx12DrawPacket(const uint8_t* packet)
{
    if (dx12::g_device)
        dx12::g_device->DrawIndexedWithConstants(packet);
}

void Dx12EngineEnvCubeFov(float tanHalf)
{
    dx12::g_engineCubeTanHalf = tanHalf;
}

void Dx12VoxSnap(const char* name)
{
    {
        std::lock_guard<std::mutex> guard(dx12::g_voxSnapLock);
        dx12::g_voxSnapName = name ? name : "";
    }
    dx12::g_voxSnapWanted = true;
}

void Dx12DumpEngineCube()
{
    dx12::g_cubeDumpWanted = true;
}

bool Dx12WaterReplacesPlanar()
{
    return dx12::g_device && dx12::Device::WaterCubeOn();
}

bool Dx12WantsEngineEnvCube()
{
    return dx12::g_device && ((dx12::Device::WaterCubeOn() && dx12::Device::EngineCubeOn()) || dx12::Device::EnvEngineOn());
}

void Dx12EngineEnvCube(ID3D10Resource* cube)
{
    std::lock_guard<std::mutex> guard(dx12::g_engineCubeLock);
    if (cube == dx12::g_engineCubePending)
        return;
    if (cube)
        cube->AddRef();
    if (dx12::g_engineCubePending)
        dx12::g_engineCubePending->Release();
    dx12::g_engineCubePending = cube;
}

bool Dx12DlssActive()
{
    return dx12::g_device && dlss::Active() && dx12::g_screenWidth.load() != 0;
}

bool Dx12ScreenSize(uint32_t* width, uint32_t* height)
{
    *width = dx12::g_screenWidth.load();
    *height = dx12::g_screenHeight.load();
    return *width && *height;
}

bool Dx12OwnsDevice(IUnknown* device)
{
    return dx12::g_device && static_cast<void*>(device) == static_cast<void*>(static_cast<ID3D10Device1*>(dx12::g_device));
}

HRESULT Dx12CreateSwapChain(IUnknown*, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    using namespace dx12;
    if (!desc || !out)
        return E_INVALIDARG;
    DXGI_SWAP_CHAIN_DESC1 d1{};
    d1.Width = desc->BufferDesc.Width;
    d1.Height = desc->BufferDesc.Height;
    d1.Format = SwapFormat(desc->BufferDesc.Format);
    d1.SampleDesc = {1, 0};
    d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d1.BufferCount = kSwapBuffers;
    d1.Scaling = DXGI_SCALING_STRETCH;
    d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    d1.Flags = (desc->Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) | (g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    fs.RefreshRate = desc->BufferDesc.RefreshRate;
    fs.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fs.Scaling = desc->BufferDesc.Scaling;
    fs.Windowed = desc->Windowed;
    const bool borderless = BorderlessOn();
    if (borderless)
        fs.Windowed = TRUE;
    // Through Streamline's factory proxy when DLSS runs, so it sees every Present.
    IDXGIFactory4* factory = dlss::ProxyFactory(g.factory);
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(g.queue, desc->OutputWindow, &d1, &fs, nullptr, &sc1);
    if (FAILED(hr) && !fs.Windowed)
    {
        Log("fullscreen swap chain failed %08lx, creating windowed", static_cast<unsigned long>(hr));
        fs.Windowed = TRUE;
        hr = factory->CreateSwapChainForHwnd(g.queue, desc->OutputWindow, &d1, &fs, nullptr, &sc1);
    }
    factory->Release();
    if (FAILED(hr))
    {
        Log("CreateSwapChainForHwnd failed %08lx", static_cast<unsigned long>(hr));
        return hr;
    }
    g.factory->MakeWindowAssociation(desc->OutputWindow, DXGI_MWA_NO_ALT_ENTER);
    auto* s = new SwapChain12;
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&s->chain));
    sc1->Release();
    s->desc = *desc;
    s->flags = d1.Flags;
    s->windowed = fs.Windowed != FALSE;
    s->borderless = borderless;
    if (borderless)
        BorderlessSetFullscreen(desc->OutputWindow, !desc->Windowed);
    if (!s->chain || !s->CreateBuffers())
    {
        Log("swap chain buffers failed");
        return E_FAIL;
    }
    Log("swap chain: %ux%u fmt %d, %u buffers, windowed %d, flags 0x%x", s->desc.BufferDesc.Width, s->desc.BufferDesc.Height, d1.Format, kSwapBuffers,
        s->windowed, d1.Flags);
    *out = s;
    return S_OK;
}

void Dx12ResetStats()
{
    dx12::g_statsResetAsked = true;
}

void Dx12WriteStats(FILE* f, double frames)
{
    using namespace dx12;
    if (!g_device)
        return;
    const Stats& s = g_device->m_stats;
    fprintf(f, "D3D12 back end, per frame: draws %.1f (packets %.1f, skipped %.1f), submissions %.2f, barriers %.1f in %.1f calls, pipeline switches %.1f, srv tables %.1f, "
               "sampler tables %.2f, splits %.2f\n",
            s.draws / frames, s.packets / frames, s.skippedDraws / frames, s.submissions / frames, s.barriers / frames, s.barrierCalls / frames,
            s.psoSwitches / frames,
            s.srvTables / frames, s.samplerTables / frames, s.splits / frames);
    UINT64 frequency = 1;
    g.queue->GetTimestampFrequency(&frequency);
    fprintf(f, "  GPU busy %.3f ms/frame (%llu lists timed), depth to SRV %.2f, clears RT %.1f DS %.1f per frame\n",
            s.gpuTicks * 1000.0 / static_cast<double>(frequency) / std::max<double>(1.0, static_cast<double>(s.frames)), static_cast<unsigned long long>(s.gpuLists),
            s.depthToSrv / frames, s.rtClears / frames, s.dsClears / frames);
    fprintf(f, "  texture copies with an empty box skipped: %llu; staging-to-staging copies as buffer rows: %llu rows\n",
            static_cast<unsigned long long>(g_emptyCopies), static_cast<unsigned long long>(g_stagingRowCopies));
    if (g_device->m_passStampsOn)
    {
        const auto& d = *g_device;
        std::vector<size_t> order(d.m_passTicks.size());
        for (size_t i = 0; i < order.size(); i++)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t c) { return d.m_passTicks[a] > d.m_passTicks[c]; });
        uint64_t sum = 0;
        for (uint64_t t : d.m_passTicks)
            sum += t;
        const double perFrame = 1000.0 / static_cast<double>(frequency) / std::max<double>(1.0, static_cast<double>(s.frames));
        fprintf(f, "  GPU time per pass (%.3f ms/frame in all, %llu stamps dropped):\n", sum * perFrame, static_cast<unsigned long long>(d.m_passStampsDropped));
        for (size_t k = 0; k < order.size() && k < 60; k++)
            fprintf(f, "    %7.3f  %s\n", d.m_passTicks[order[k]] * perFrame, d.m_passLabelNames[order[k]].c_str());
    }
    fprintf(f, "  pipeline states %llu (failed %llu, %.1f ms on the context thread), sampler heap resets %llu, presented frames %llu\n",
            static_cast<unsigned long long>(s.psoCreated), static_cast<unsigned long long>(s.psoFailed), s.psoMs, static_cast<unsigned long long>(s.samplerResets),
            static_cast<unsigned long long>(s.frames));
    {
        std::lock_guard<std::mutex> guard(g_pipes.lock);
        fprintf(f, "  pipelines ahead: %llu recipes on disk, %llu created by %d workers (%.1f ms), %llu taken ready, %llu waited for (%.1f ms), %llu claimed "
                   "from the queue, %llu created at draw (%llu failed), %zu still queued; created in the background at draw %llu, draws skipped meanwhile %llu\n",
                static_cast<unsigned long long>(g_pipes.onDisk), static_cast<unsigned long long>(g_pipes.precreated), g_pipes.workers, g_pipes.workerMs,
                static_cast<unsigned long long>(g_pipes.taken), static_cast<unsigned long long>(g_pipes.waited), g_pipes.waitMs,
                static_cast<unsigned long long>(g_pipes.claimed), static_cast<unsigned long long>(g_pipes.created), static_cast<unsigned long long>(g_pipes.failed),
                g_pipes.queue.size(), static_cast<unsigned long long>(g_pipes.deferred), static_cast<unsigned long long>(g_pipes.deferredDraws));
    }
    dlss::WriteStats(f);
    fprintf(f, "  object motion: the motion blur target copied after its pass %llu times; bent vegetation's motion in %llu motion passes\n",
            static_cast<unsigned long long>(s.objectMotionCopies), static_cast<unsigned long long>(s.bendMotionPasses));
    fprintf(f, "  DLSS bias mask (water, particles): frames %llu, draws %llu, given to DLSS %llu\n", static_cast<unsigned long long>(s.biasFrames),
            static_cast<unsigned long long>(s.biasDraws), static_cast<unsigned long long>(s.biasGiven));
    fprintf(f, "  exact object motion (the patched motion blur pass): frames %llu, draws %llu, read by the motion pass %llu\n",
            static_cast<unsigned long long>(s.objectMotionExactFrames), static_cast<unsigned long long>(s.objectMotionExactDraws),
            static_cast<unsigned long long>(s.objectMotionExactPasses));
    fprintf(f, "  scenes %llu, DLSS applied %llu, motion vectors %llu (with object velocities %llu), GI %llu, voxel updates %llu, GI reads after the scene %llu, with another depth or target %llu\n", static_cast<unsigned long long>(s.scenes),
            static_cast<unsigned long long>(s.dlssFrames), static_cast<unsigned long long>(s.motionPasses), static_cast<unsigned long long>(s.objectMotionPasses),
            static_cast<unsigned long long>(s.giPasses), static_cast<unsigned long long>(s.voxPasses), static_cast<unsigned long long>(s.giLateReads),
            static_cast<unsigned long long>(s.giOtherDepth));
    fprintf(f, "  GI geometry: z-prepass draws voxelized %.1f per frame (with their material %.1f), foliage left out %.1f, skipped without the camera matrix %.1f, other %.1f, without a pipeline %.1f; pipelines %llu\n",
            s.voxGeomDraws / frames, s.voxGeomMaterial / frames, s.voxGeomFoliage / frames, s.voxGeomNoCamera / frames, s.voxGeomSkipped / frames, s.voxGeomNoPso / frames, static_cast<unsigned long long>(s.voxGeomPsos));
    fprintf(f, "  GI geometry, z-prepass draws by their pixel shader: plain %.1f, diffuse map only %.1f, discard only (LOD dissolve) %.1f, discard and diffuse map (alpha test: foliage) %.1f per frame\n",
            s.voxClass[0] / frames, s.voxClass[1] / frames, s.voxClass[2] / frames, s.voxClass[3] / frames);
    fprintf(f, "  GI geometry: solid draws voxelized into level 0 %.1f per frame; replays skipped as their list was done %.1f per frame (%llu in all)\n",
            s.voxGeomSolid / frames, s.voxGeomStale / frames, static_cast<unsigned long long>(s.voxGeomStale));
    fprintf(f, "  GI geometry cache: animated %.1f, new %.1f per frame; level draws left to their turn %.1f per frame\n",
            s.voxGeomAnimated / frames, s.voxGeomFresh / frames, s.voxGeomInTurn / frames);
    fprintf(f, "  GI geometry cache, animated because: layout (bones or per-instance) %.1f, dynamic vertex buffer %.1f, no object buffer %.1f, its matrix not known %.1f per frame\n",
            s.voxAnimatedWhy[0] / frames, s.voxAnimatedWhy[1] / frames, s.voxAnimatedWhy[2] / frames, s.voxAnimatedWhy[3] / frames);
    fprintf(f, "  GI geometry, solid z-prepass draws by their camera matrix: none %.1f, absolute %.1f, absolute transposed %.1f, relative %.1f, relative transposed %.1f per frame\n",
            s.voxPatch[0] / frames, s.voxPatch[1] / frames, s.voxPatch[2] / frames, s.voxPatch[3] / frames, s.voxPatch[4] / frames);
    fprintf(f, "  drawn: %.2f M vertices/indices per frame (all passes; triangle lists: / 3)\n", s.vertices / frames / 1e6);
    fprintf(f, "  foliage mask: written in %llu frames; R32F draws with depth %.1f per frame, with SV_Target1 %.1f\n", static_cast<unsigned long long>(s.foliageFrames),
            s.zpassDraws / frames, s.zpassTarget1 / frames);
    fprintf(f, "  water SSR: depth pyramid built in %llu frames\n", static_cast<unsigned long long>(s.hizRuns));
    fprintf(f, "  volumetric fog (GI in the fog): made in %llu frames; left to the engine: no GI volumes %llu, no fog constants %llu, other target %llu\n",
            static_cast<unsigned long long>(s.vfogRuns), static_cast<unsigned long long>(s.vfogNoVolume), static_cast<unsigned long long>(s.vfogNoConstants),
            static_cast<unsigned long long>(s.vfogOtherTarget));
    fprintf(f, "  volumetric fog sun shadows: frames with the sun's cascades %llu, %.2f cascades a frame\n", static_cast<unsigned long long>(s.vfogCascadeFrames),
            s.vfogCascadeFrames ? static_cast<double>(s.vfogCascadeSum) / static_cast<double>(s.vfogCascadeFrames) : 0.0);
    fprintf(f, "  engine cube: the slice each face number of the start mark ends in (0-5): %d %d %d %d %d %d (changed %llu times)\n", g_cubeSliceOfMark[0],
            g_cubeSliceOfMark[1], g_cubeSliceOfMark[2], g_cubeSliceOfMark[3], g_cubeSliceOfMark[4], g_cubeSliceOfMark[5],
            static_cast<unsigned long long>(g_cubeSliceLearned));
    fprintf(f, "  engine environment cube: %llu draws into it (faces %llu %llu %llu %llu %llu %llu), its source let go as a target %llu times; HDR copy updates %llu\n",
            static_cast<unsigned long long>(s.engineCubeDraws), static_cast<unsigned long long>(s.engineCubeFaces[0]),
            static_cast<unsigned long long>(s.engineCubeFaces[1]), static_cast<unsigned long long>(s.engineCubeFaces[2]),
            static_cast<unsigned long long>(s.engineCubeFaces[3]), static_cast<unsigned long long>(s.engineCubeFaces[4]),
            static_cast<unsigned long long>(s.engineCubeFaces[5]), static_cast<unsigned long long>(s.engineCubeUnbinds),
            static_cast<unsigned long long>(s.engineCubeHdrRuns));
    fprintf(f, "  screen-space shadows (Bend): made in %llu frames, %.2f dispatches each; frames without the z-prepass target %llu\n",
            static_cast<unsigned long long>(s.sssRuns), s.sssRuns ? static_cast<double>(s.sssDispatches) / s.sssRuns : 0.0,
            static_cast<unsigned long long>(s.sssNoDepth));
    fprintf(f, "  GI geometry: first-person draws (weapon, hands) left out %.1f per frame\n", s.voxGeomWeapon / frames);
    if (s.voxInjectRuns || s.voxInjectSkipped)
        fprintf(f, "  GI cells from the engine cube's faces (inject): faces %llu (%.2f per frame), skipped (depth and colour sizes differ) %llu\n",
                static_cast<unsigned long long>(s.voxInjectRuns), s.voxInjectRuns / frames, static_cast<unsigned long long>(s.voxInjectSkipped));
    if (s.voxCubeSeen)
        fprintf(f, "  GI geometry from the engine cube's faces: draws seen %.1f per frame, recorded %.1f, without the face camera %.1f, "
                   "foliage left out %.1f, repeats %.1f; jobs %llu, voxelized %.1f per frame\n",
                s.voxCubeSeen / frames, s.voxCubeRecorded / frames, s.voxCubeNoCamera / frames, s.voxCubeFoliage / frames,
                s.voxCubeRepeat / frames, static_cast<unsigned long long>(s.voxCubeJobs), s.voxCubeDraws / frames);
    if (s.voxShadowJobs)
    {
        fprintf(f, "  GI geometry from shadow maps: maps %.2f per frame, jobs %llu (level covered %llu), draws recorded %.1f, voxelized %.1f, without the light matrix %.1f; last frame's maps cover",
                s.voxShadowMaps / frames, static_cast<unsigned long long>(s.voxShadowJobs), static_cast<unsigned long long>(s.voxShadowCovered),
                s.voxShadowRecorded / frames, s.voxShadowDraws / frames, s.voxShadowNoCamera / frames);
        for (uint32_t i = 0; i < s.voxShadowCoverCount && i < 8; i++)
            fprintf(f, " %.1f", s.voxShadowCover[i]);
        fprintf(f, " m\n   per map (draws a frame drawn, mean cover m):");
        for (int i = 0; i < 8; i++)
            if (s.voxShadowMapSeen[i])
                fprintf(f, " %d: %.1f, %.1f;", i, static_cast<double>(s.voxShadowMapDraws[i]) / static_cast<double>(s.voxShadowMapSeen[i]),
                        s.voxShadowMapCover[i] / static_cast<double>(s.voxShadowMapSeen[i]));
        fprintf(f, "\n");
    }
    if (s.giStampRuns)
    {
        const double ms = 1000.0 / static_cast<double>(frequency) / static_cast<double>(s.giStampRuns);
        fprintf(f, "  GI passes, GPU ms a run (%llu runs): trace %.3f, filter %.3f, upsample %.3f, voxel geometry %.3f, voxel update %.3f\n",
                static_cast<unsigned long long>(s.giStampRuns), s.giStampTicks[0] * ms, s.giStampTicks[1] * ms, s.giStampTicks[2] * ms,
                s.giStampTicks[3] * ms, s.giStampTicks[4] * ms);
        if (s.giProbeRuns)
            fprintf(f, "  GI v2 probes: %llu runs (their pass is inside the trace's time)\n", static_cast<unsigned long long>(s.giProbeRuns));
    }
    fprintf(f, "  clears per frame:\n");
    for (const auto& [key, count] : g_device->m_clears)
        if (count / frames >= 0.05)
            fprintf(f, "    %6.2f  %s\n", count / frames, key.c_str());
    fprintf(f, "  Map(DISCARD) per frame: constant buffers %.0f (%.2f MB), vertex/index buffers %.0f (%.2f MB), other %.0f (%.2f MB)\n", g_renames[0] / frames,
            g_renameBytes[0] / frames / 1048576.0, g_renames[1] / frames, g_renameBytes[1] / frames / 1048576.0, g_renames[2] / frames,
            g_renameBytes[2] / frames / 1048576.0);
    fprintf(f, "  barriers per frame:\n");
    for (const auto& [key, count] : g_device->m_barrierKinds)
        if (count / frames >= 0.05)
            fprintf(f, "    %6.2f  %s\n", count / frames, key.c_str());
    fprintf(f, "  memory: upload slice pages %llu (+%llu dedicated), default buffer pages %llu (+%llu dedicated), creation uploads %.1f MB in %llu upload lists\n",
            static_cast<unsigned long long>(g_slices.pages), static_cast<unsigned long long>(g_slices.dedicated), static_cast<unsigned long long>(g_blocks.pages),
            static_cast<unsigned long long>(g_blocks.dedicated), g_uploadBytes / 1048576.0, static_cast<unsigned long long>(g_uploadSubmits));
}
