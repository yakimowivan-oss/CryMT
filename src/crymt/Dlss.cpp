// DLSS through NVIDIA Streamline (SDK in D:\Games\Crysis-RE\sdk\streamline-*, runtime DLLs in the streamline folder
// next to CryMT.dll). Manual hooking: Streamline gets our D3D12 device, and the command queue and swap chain
// are created through its proxies so it sees every Present. DLAA runs on the scene's HDR color before the renderer's HDR
// post-processing; camera motion is derived by Streamline from depth and the matrices (no object motion yet).
// Over-the-air updates are off: nothing is downloaded at run time.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>

#include <cmath>
#include <cstring>

#include <string>

#include "Dlss.h"
#include "GiControl.h"
#include "Paths.h"

namespace dlss
{
namespace
{
// Identifies this integration to NGX in place of an application id.
const char* const kProjectId = "6f3c2b1e-9a4d-4c8e-b7f2-1d5e8a9c0b37";

void (*g_log)(const char*);
bool g_active;
PFun_slInit* g_slInit;
PFun_slFreeResources* g_slFreeResources; // optional: the comparison's second DLSS let go
PFun_slSetD3DDevice* g_slSetD3DDevice;
PFun_slIsFeatureSupported* g_slIsFeatureSupported;
PFun_slGetNewFrameToken* g_slGetNewFrameToken;
PFun_slSetConstants* g_slSetConstants;
PFun_slSetTagForFrame* g_slSetTagForFrame;
PFun_slEvaluateFeature* g_slEvaluateFeature;
PFun_slUpgradeInterface* g_slUpgradeInterface;
PFun_slGetFeatureFunction* g_slGetFeatureFunction;
PFun_slDLSSSetOptions* g_slDLSSSetOptions;
// Streamline's proxies are kept for the life of the device: the command queue proxy points back at the device
// proxy without a reference, and releasing the device proxy after CreateCommandQueue left the factory proxy's
// CreateSwapChainForHwnd reading freed memory (about 1 start in 10 faulted at sl.interposer+0x2A063).
ID3D12Device* g_deviceProxy;
IDXGIFactory4* g_factoryProxy;

uint32_t g_optionsWidth, g_optionsHeight;
uint64_t g_evaluations, g_failures, g_resets;
sl::float4x4 g_prevViewToWorld, g_prevProj;
float g_prevView[16]; // the previous frame's view (the cut test by the turn)
bool g_havePrev;

void Logf(const char* format, ...)
{
    if (!g_log)
        return;
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_log(text);
}

void OnSlLog(sl::LogType type, const char* message)
{
    if (type != sl::LogType::eInfo)
        Logf("streamline: %s", message);
}

// ---- matrices (row vectors, rows in memory, as both the engine and Streamline use them)
sl::float4x4 Load(const float* m)
{
    sl::float4x4 r;
    for (int i = 0; i < 4; i++)
        r[i] = sl::float4(m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]);
    return r;
}

sl::float4x4 Mul(const sl::float4x4& a, const sl::float4x4& b)
{
    const float* pa = &a.row[0].x;
    const float* pb = &b.row[0].x;
    sl::float4x4 r;
    float* pr = &r.row[0].x;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += pa[i * 4 + k] * pb[k * 4 + j];
            pr[i * 4 + j] = s;
        }
    return r;
}

sl::float4x4 Invert(const sl::float4x4& mat)
{
    double m[16], inv[16];
    const float* p = &mat.row[0].x;
    for (int i = 0; i < 16; i++)
        m[i] = p[i];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    const double s = det != 0 ? 1.0 / det : 0.0;
    sl::float4x4 r;
    float* pr = &r.row[0].x;
    for (int i = 0; i < 16; i++)
        pr[i] = static_cast<float>(inv[i] * s);
    return r;
}

// Camera to previous camera with the current camera at the origin (Streamline's helper, for fp32 precision).
sl::float4x4 CameraToPrevCamera(const sl::float4x4& cameraToWorld, const sl::float4x4& cameraToWorldPrev)
{
    sl::float4x4 cc = cameraToWorld;
    cc[3] = sl::float4(0, 0, 0, 1);
    sl::float4x4 ccPrev = cameraToWorldPrev;
    ccPrev[3].x -= cameraToWorld[3].x;
    ccPrev[3].y -= cameraToWorld[3].y;
    ccPrev[3].z -= cameraToWorld[3].z;
    return Mul(cc, Invert(ccPrev));
}

sl::float4x4 Identity()
{
    sl::float4x4 r;
    for (int i = 0; i < 4; i++)
        r[i] = sl::float4(i == 0, i == 1, i == 2, i == 3);
    return r;
}

template <class T>
bool Resolve(HMODULE module, const char* name, T*& out)
{
    out = reinterpret_cast<T*>(GetProcAddress(module, name));
    if (!out)
        Logf("streamline: %s missing", name);
    return out != nullptr;
}
} // namespace

bool Init(ID3D12Device* device, const LUID& adapter, void (*log)(const char*))
{
    g_log = log;
    static const std::wstring runtimeDir = CryMTDirW() + L"streamline";
    static const std::wstring logDir = CryMTDirW() + L"logs\\mt";
    CryMTLog(""); // creates logs\mt
    const wchar_t* const kRuntimeDir = runtimeDir.c_str();
    const wchar_t* const kLogDir = logDir.c_str();
    const std::wstring path = runtimeDir + L"\\sl.interposer.dll";
    HMODULE sl = LoadLibraryW(path.c_str());
    if (!sl)
    {
        Logf("streamline: sl.interposer.dll not loaded (%lu)", GetLastError());
        return false;
    }
    Resolve(sl, "slFreeResources", g_slFreeResources);
    if (!Resolve(sl, "slInit", g_slInit) || !Resolve(sl, "slSetD3DDevice", g_slSetD3DDevice) || !Resolve(sl, "slIsFeatureSupported", g_slIsFeatureSupported) ||
        !Resolve(sl, "slGetNewFrameToken", g_slGetNewFrameToken) || !Resolve(sl, "slSetConstants", g_slSetConstants) ||
        !Resolve(sl, "slSetTagForFrame", g_slSetTagForFrame) || !Resolve(sl, "slEvaluateFeature", g_slEvaluateFeature) ||
        !Resolve(sl, "slUpgradeInterface", g_slUpgradeInterface) || !Resolve(sl, "slGetFeatureFunction", g_slGetFeatureFunction))
        return false;

    const wchar_t* plugins[] = {kRuntimeDir};
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugins;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = kLogDir;
    pref.logMessageCallback = &OnSlLog;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "CryEngine 2 (Crysis 1.2.1)";
    pref.projectId = kProjectId;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = g_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk)
    {
        Logf("streamline: slInit failed %d", static_cast<int>(r));
        return false;
    }
    r = g_slSetD3DDevice(device);
    if (r != sl::Result::eOk)
    {
        Logf("streamline: slSetD3DDevice failed %d", static_cast<int>(r));
        return false;
    }
    sl::AdapterInfo info{};
    LUID luid = adapter;
    info.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    info.deviceLUIDSizeInBytes = sizeof(LUID);
    r = g_slIsFeatureSupported(sl::kFeatureDLSS, info);
    if (r != sl::Result::eOk)
    {
        Logf("streamline: DLSS not supported on this adapter (%d)", static_cast<int>(r));
        return false;
    }
    void* fn = nullptr;
    if (g_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn) != sl::Result::eOk || !fn)
    {
        Logf("streamline: slDLSSSetOptions missing");
        return false;
    }
    g_slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
    g_active = true;
    Logf("streamline: DLSS ready (SDK %d.%d.%d)", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    return true;
}

bool Active()
{
    return g_active;
}

HRESULT CreateQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, ID3D12CommandQueue** queue)
{
    if (!g_active)
        return device->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue));
    if (!g_deviceProxy)
    {
        ID3D12Device* proxy = device;
        proxy->AddRef();
        if (g_slUpgradeInterface(reinterpret_cast<void**>(&proxy)) != sl::Result::eOk)
        {
            proxy->Release();
            Logf("streamline: device proxy failed");
            return device->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue));
        }
        g_deviceProxy = proxy;
    }
    return g_deviceProxy->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue));
}

IDXGIFactory4* ProxyFactory(IDXGIFactory4* factory)
{
    if (g_active && !g_factoryProxy)
    {
        IDXGIFactory4* proxy = factory;
        proxy->AddRef();
        if (g_slUpgradeInterface(reinterpret_cast<void**>(&proxy)) == sl::Result::eOk)
            g_factoryProxy = proxy;
        else
        {
            proxy->Release();
            Logf("streamline: factory proxy failed");
        }
    }
    IDXGIFactory4* result = g_factoryProxy ? g_factoryProxy : factory;
    result->AddRef();
    return result;
}

// The DLAA model (2026-11-13: the panel switches it live; the launcher sets CRYMT_DLSS_PRESET): K the transformer (the
// default), J (a little less ghosting, more flicker), L and M the second transformer generation of DLSS 4.5 (sharper,
// steadier, less ghosting; dearer), F and E the old CNN models.
const char kPresetLetters[] = "KJLMFE";
int PresetIndex()
{
    const int panel = g_giControl.dlssPreset.load();
    if (panel >= 0 && panel < 6)
        return panel;
    char text[8] = {};
    if (GetEnvironmentVariableA("CRYMT_DLSS_PRESET", text, sizeof(text)))
        for (int i = 0; i < 6; i++)
            if ((text[0] & ~0x20) == kPresetLetters[i])
                return i;
    return 0;
}

int PresetDefaultIndex()
{
    char text[8] = {};
    if (GetEnvironmentVariableA("CRYMT_DLSS_PRESET", text, sizeof(text)))
        for (int i = 0; i < 6; i++)
            if ((text[0] & ~0x20) == kPresetLetters[i])
                return i;
    return 0;
}

sl::DLSSOptions g_options{};      // the main DLSS's, for the comparison's copy
uint64_t g_compareEvaluations;      // the comparison's second DLSS (viewport 1)
bool g_compareLive;                 // it has resources

// The model comparison: the same inputs into a second DLSS (viewport 1, its own history) with another model, into
// images.output2. Called after the main one, with its frame token and constants.
void EvaluateCompare(const Images& images, sl::FrameToken& token, const sl::Constants& c, const sl::ResourceTag* tags, uint32_t tagCount,
                     sl::CommandBuffer* cmd)
{
    static int optionsPreset = -1;
    static uint32_t optionsWidth, optionsHeight;
    const int compare = g_giControl.dlssCompare.load();
    const sl::ViewportHandle viewport(1);
    if (compare < 0 || compare >= 6 || !images.output2)
    {
        if (g_compareLive && g_slFreeResources)
            g_slFreeResources(sl::kFeatureDLSS, viewport);
        g_compareLive = false;
        optionsPreset = -1;
        return;
    }
    if (compare != optionsPreset || images.width != optionsWidth || images.height != optionsHeight)
    {
        sl::DLSSOptions options = g_options;
        options.dlaaPreset = static_cast<sl::DLSSPreset>(kPresetLetters[compare] - 'A' + 1);
        if (g_slDLSSSetOptions(viewport, options) != sl::Result::eOk)
        {
            if (g_failures++ < 5)
                Logf("streamline: comparison options failed");
            return;
        }
        optionsPreset = compare;
        optionsWidth = images.width;
        optionsHeight = images.height;
        Logf("streamline: comparison DLAA preset %u on the right half", static_cast<unsigned>(options.dlaaPreset));
    }
    if (g_slSetConstants(c, token, viewport) != sl::Result::eOk)
        return;
    sl::Resource output(sl::ResourceType::eTex2d, images.output2, static_cast<uint32_t>(images.output2State));
    sl::ResourceTag mine[6];
    for (uint32_t i = 0; i < tagCount && i < 6; i++)
        mine[i] = tags[i];
    const sl::Extent extent{0, 0, images.width, images.height};
    mine[1] = sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &extent);
    if (g_slSetTagForFrame(token, viewport, mine, tagCount, cmd) != sl::Result::eOk)
        return;
    const sl::BaseStructure* inputs[] = {&viewport};
    if (g_slEvaluateFeature(sl::kFeatureDLSS, token, inputs, 1, cmd) == sl::Result::eOk)
    {
        g_compareEvaluations++;
        g_compareLive = true;
    }
    else if (g_failures++ < 5)
        Logf("streamline: comparison evaluate failed");
}

bool Evaluate(ID3D12GraphicsCommandList* list, const Images& images, const DlssSceneInfo& scene)
{
    if (!g_active)
        return false;
    const sl::ViewportHandle viewport(0);
    static int optionsExposure = -1; // the exposure source the options were set for
    static int optionsPreset = -1;
    const int ownExposure = images.exposure ? 1 : 0;
    const int preset = PresetIndex();
    if (images.width != g_optionsWidth || images.height != g_optionsHeight || ownExposure != optionsExposure || preset != optionsPreset)
    {
        sl::DLSSOptions options{};
        options.mode = sl::DLSSMode::eDLAA;
        options.outputWidth = images.width;
        options.outputHeight = images.height;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        // Exposure: CryMT's (images.exposure, the scene's average luminance like an eye adaptation), else DLSS's auto
        // exposure, which left night shade almost black to it: white single-pixel sparkles there (island 4:30, ground
        // in shade: ~210 a frame, a fixed exposure x20 ~34). Tests: CRYMT_DLSS_AUTOEXPOSURE=0 (fixed 1),
        // CRYMT_DLSS_EXPOSURESCALE (times the exposure).
        char text[32] = {};
        const bool autoExposure = !ownExposure && !(GetEnvironmentVariableA("CRYMT_DLSS_AUTOEXPOSURE", text, sizeof(text)) && strcmp(text, "0") == 0);
        options.useAutoExposure = autoExposure ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        if (GetEnvironmentVariableA("CRYMT_DLSS_EXPOSURESCALE", text, sizeof(text)))
            options.exposureScale = static_cast<float>(atof(text));
        options.dlaaPreset = static_cast<sl::DLSSPreset>(kPresetLetters[preset] - 'A' + 1);
        Logf("streamline: DLAA preset %u", static_cast<unsigned>(options.dlaaPreset));
        Logf("streamline: auto exposure %d, exposure scale %g", autoExposure ? 1 : 0, options.exposureScale);
        const sl::Result r = g_slDLSSSetOptions(viewport, options);
        if (r != sl::Result::eOk)
        {
            Logf("streamline: slDLSSSetOptions failed %d", static_cast<int>(r));
            g_failures++;
            return false;
        }
        g_options = options;
        g_optionsWidth = images.width;
        g_optionsHeight = images.height;
        optionsExposure = ownExposure;
        optionsPreset = preset;
        g_havePrev = false;
        Logf("streamline: DLAA %ux%u", images.width, images.height);
    }

    sl::FrameToken* token = nullptr;
    const uint32_t frameIndex = scene.frame;
    if (g_slGetNewFrameToken(token, &frameIndex) != sl::Result::eOk || !token)
    {
        g_failures++;
        return false;
    }

    // Matrices without jitter; the view is orthonormal, so its inverse is the camera's placement in the world.
    const sl::float4x4 view = Load(scene.view);
    const sl::float4x4 proj = Load(scene.proj);
    sl::float4x4 viewToWorld = Invert(view);
    // The camera's place from the CCamera (the inverse of the view's float translation is off by ~0.2 mm 2 km from the
    // world's origin, a different amount each frame: see D3D12Backend ReprojDouble).
    if (scene.camPos[3] == 1.0f)
        viewToWorld[3] = sl::float4(scene.camPos[0], scene.camPos[1], scene.camPos[2], 1.0f);
    // A camera cut (teleport, level start, cutscene) has no usable history.
    const float dx = viewToWorld[3].x - g_prevViewToWorld[3].x, dy = viewToWorld[3].y - g_prevViewToWorld[3].y, dz = viewToWorld[3].z - g_prevViewToWorld[3].z;
    // A cut also when the camera turned more than 45 degrees in one frame (a cutscene's cut that only turns).
    double turn = 0.0;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            turn += static_cast<double>(scene.view[r * 4 + k]) * g_prevView[r * 4 + k];
    const bool reset = scene.reset || !g_havePrev || dx * dx + dy * dy + dz * dz > 100.0f || (turn - 1.0) / 2.0 < 0.7071;
    sl::Constants c{};
    c.cameraViewToClip = proj;
    c.clipToCameraView = Invert(proj);
    c.clipToLensClip = Identity();
    const sl::float4x4 cameraToPrev = reset ? Identity() : CameraToPrevCamera(viewToWorld, g_prevViewToWorld);
    c.clipToPrevClip = Mul(Mul(c.clipToCameraView, cameraToPrev), reset ? proj : g_prevProj);
    c.prevClipToClip = Invert(c.clipToPrevClip);
    c.jitterOffset = sl::float2(scene.jitterX, scene.jitterY);
    // Our motion vectors are in UV units (fractions of the screen), Streamline's normalized range: scale 1.
    // CRYMT_DLSS_MVSCALE (tests): a factor on both axes (2: NDC units, 0: no motion given to DLSS).
    static const float mvScale = [] {
        char text[32] = {};
        return GetEnvironmentVariableA("CRYMT_DLSS_MVSCALE", text, sizeof(text)) ? static_cast<float>(atof(text)) : 1.0f;
    }();
    c.mvecScale = sl::float2(mvScale, mvScale);
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    c.cameraPos = sl::float3(viewToWorld[3].x, viewToWorld[3].y, viewToWorld[3].z);
    c.cameraRight = sl::float3(viewToWorld[0].x, viewToWorld[0].y, viewToWorld[0].z);
    c.cameraUp = sl::float3(viewToWorld[1].x, viewToWorld[1].y, viewToWorld[1].z);
    c.cameraFwd = sl::float3(-viewToWorld[2].x, -viewToWorld[2].y, -viewToWorld[2].z); // right-handed: the view looks down -z
    c.cameraNear = scene.nearZ;
    c.cameraFar = scene.farZ;
    c.cameraFOV = scene.fov;
    c.cameraAspectRatio = scene.aspect;
    c.motionVectorsInvalidValue = 1.0e9f; // never in our motion vectors
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = images.cameraMotion ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    if (g_slSetConstants(c, *token, viewport) != sl::Result::eOk)
    {
        g_failures++;
        return false;
    }
    g_prevViewToWorld = viewToWorld;
    g_prevProj = proj;
    memcpy(g_prevView, scene.view, sizeof(g_prevView));
    g_havePrev = true;
    if (reset)
        g_resets++;

    sl::Resource color(sl::ResourceType::eTex2d, images.color, static_cast<uint32_t>(images.colorState));
    sl::Resource output(sl::ResourceType::eTex2d, images.output, static_cast<uint32_t>(images.outputState));
    sl::Resource depth(sl::ResourceType::eTex2d, images.depth, static_cast<uint32_t>(images.depthState));
    sl::Resource mvec(sl::ResourceType::eTex2d, images.mvec, static_cast<uint32_t>(images.mvecState));
    const sl::Extent extent{0, 0, images.width, images.height};
    sl::Resource exposure(sl::ResourceType::eTex2d, images.exposure, static_cast<uint32_t>(images.exposureState));
    const sl::Extent one{0, 0, 1, 1};
    sl::Resource bias(sl::ResourceType::eTex2d, images.bias, static_cast<uint32_t>(images.biasState));
    sl::ResourceTag tags[6] = {
        sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent),
    };
    uint32_t tagCount = 4;
    if (images.exposure)
        tags[tagCount++] = sl::ResourceTag(&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent, &one);
    if (images.bias) // water and particles: lerp(history, current, mask)
        tags[tagCount++] = sl::ResourceTag(&bias, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &extent);
    auto* cmd = reinterpret_cast<sl::CommandBuffer*>(list);
    if (g_slSetTagForFrame(*token, viewport, tags, tagCount, cmd) != sl::Result::eOk)
    {
        g_failures++;
        return false;
    }
    const sl::BaseStructure* inputs[] = {&viewport};
    const sl::Result r = g_slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, cmd);
    if (r != sl::Result::eOk)
    {
        if (g_failures++ < 5)
            Logf("streamline: slEvaluateFeature failed %d", static_cast<int>(r));
        return false;
    }
    g_evaluations++;
    EvaluateCompare(images, *token, c, tags, tagCount, cmd);
    return true;
}

void WriteStats(FILE* f)
{
    fprintf(f, "  dlss: active %d, evaluations %llu, failures %llu, history resets %llu; model comparison (right half) %llu\n", g_active,
            static_cast<unsigned long long>(g_evaluations), static_cast<unsigned long long>(g_failures), static_cast<unsigned long long>(g_resets),
            static_cast<unsigned long long>(g_compareEvaluations));
}
} // namespace dlss
