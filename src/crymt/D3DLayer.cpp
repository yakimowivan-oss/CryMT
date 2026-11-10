// D3D layer: the way to a D3D12 back end for Crysis's D3D10 renderer (CryRenderD3D10.dll).
// The renderer gets D3D10CreateDevice through GetProcAddress on d3d10.dll and CreateDXGIFactory on dxgi.dll; D3DX10
// (d3dx10_34.dll) creates textures through the device it is given. Stage A0 (CRYMT_D3DCENSUS=1) counts every
// ID3D10Device call and records what is created and how resources are mapped, to size the D3D12 translation.
#include <windows.h>
#include <d3d10_1.h>
#include <d3d10shader.h>

#include "D3D12Backend.h"
#include "DrawPacket.h"
#include "Paths.h"
#include "DlssScene.h"

// CryMT: the renderer's modelview and projection (16 floats each) when they changed since the last call (render thread).
bool CryMtPassMatrices(float* out);

void CryMTInstallPakLog(); // CryMT.cpp: CRYMT_PAKLOG
void CryMTForceNoMsaa();   // CryMT.cpp: MSAA is not supported with CryMT yet

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// From CryMT.cpp.
void* CryMTPatchModuleExport(uintptr_t base, const char* name, const void* hook);
int CryMTPatchImports(uintptr_t base, const char* dll, const char* const* names, const void* const* hooks, int count);
void CryMTSetupLog(const char* text);
#define PatchModuleExport CryMTPatchModuleExport
#define SetupLog CryMTSetupLog

namespace
{
constexpr int kDeviceSlots = 98;
constexpr int kMapSlot = 10; // ID3D10Buffer / ID3D10Texture2D
const char* const kDeviceNames[kDeviceSlots] = {
    "QueryInterface", "AddRef", "Release", "VSSetConstantBuffers", "PSSetShaderResources", "PSSetShader", "PSSetSamplers",
    "VSSetShader", "DrawIndexed", "Draw", "PSSetConstantBuffers", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer",
    "DrawIndexedInstanced", "DrawInstanced", "GSSetConstantBuffers", "GSSetShader", "IASetPrimitiveTopology",
    "VSSetShaderResources", "VSSetSamplers", "SetPredication", "GSSetShaderResources", "GSSetSamplers", "OMSetRenderTargets",
    "OMSetBlendState", "OMSetDepthStencilState", "SOSetTargets", "DrawAuto", "RSSetState", "RSSetViewports",
    "RSSetScissorRects", "CopySubresourceRegion", "CopyResource", "UpdateSubresource", "ClearRenderTargetView",
    "ClearDepthStencilView", "GenerateMips", "ResolveSubresource", "VSGetConstantBuffers", "PSGetShaderResources",
    "PSGetShader", "PSGetSamplers", "VSGetShader", "PSGetConstantBuffers", "IAGetInputLayout", "IAGetVertexBuffers",
    "IAGetIndexBuffer", "GSGetConstantBuffers", "GSGetShader", "IAGetPrimitiveTopology", "VSGetShaderResources",
    "VSGetSamplers", "GetPredication", "GSGetShaderResources", "GSGetSamplers", "OMGetRenderTargets", "OMGetBlendState",
    "OMGetDepthStencilState", "SOGetTargets", "RSGetState", "RSGetViewports", "RSGetScissorRects",
    "GetDeviceRemovedReason", "SetExceptionMode", "GetExceptionMode", "GetPrivateData", "SetPrivateData",
    "SetPrivateDataInterface", "ClearState", "Flush", "CreateBuffer", "CreateTexture1D", "CreateTexture2D",
    "CreateTexture3D", "CreateShaderResourceView", "CreateRenderTargetView", "CreateDepthStencilView", "CreateInputLayout",
    "CreateVertexShader", "CreateGeometryShader", "CreateGeometryShaderWithStreamOutput", "CreatePixelShader",
    "CreateBlendState", "CreateDepthStencilState", "CreateRasterizerState", "CreateSamplerState", "CreateQuery",
    "CreatePredicate", "CreateCounter", "CheckFormatSupport", "CheckMultisampleQualityLevels", "CheckCounterInfo",
    "CheckCounter", "GetCreationFlags", "OpenSharedResource", "SetTextFilterSize", "GetTextFilterSize"};

bool g_censusOn;

// Appends a line to logs\mt\d3dlayer.txt at once (device creation happens before CryMT's first stats write).
void Trace(const char* text)
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("d3dlayer.txt").c_str(), "a") == 0 && f)
    {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}
volatile LONG64 g_deviceCalls[kDeviceSlots];
uint64_t g_censusFrames;
CRITICAL_SECTION g_censusLock;
std::map<std::string, uint64_t> g_created; // creation descriptions
std::map<std::string, uint64_t> g_maps;    // resource kind, map type, flags

void Note(std::map<std::string, uint64_t>& m, const char* key)
{
    EnterCriticalSection(&g_censusLock);
    m[key]++;
    LeaveCriticalSection(&g_censusLock);
}

// Counting stub: movabs rax, &counter; lock inc qword [rax]; movabs rax, original; jmp rax.
uint8_t* EmitCountStub(uint8_t* p, volatile LONG64* counter, uintptr_t original)
{
    const auto c = reinterpret_cast<uint64_t>(counter);
    *p++ = 0x48; *p++ = 0xB8; memcpy(p, &c, 8); p += 8;
    *p++ = 0xF0; *p++ = 0x48; *p++ = 0xFF; *p++ = 0x00;
    *p++ = 0x48; *p++ = 0xB8; memcpy(p, &original, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xE0;
    return p;
}

void PatchSlot(uintptr_t* vtable, int slot, uintptr_t value)
{
    DWORD old;
    VirtualProtect(&vtable[slot], 8, PAGE_READWRITE, &old);
    vtable[slot] = value;
    VirtualProtect(&vtable[slot], 8, old, &old);
}

// The renderer's probe devices and its final (single-threaded) device may use different implementation vtables; each
// is patched once and keeps its own originals, found again through the object's vtable.
struct PatchedVtable
{
    uintptr_t* vtable;
    uintptr_t orig[kDeviceSlots];
};
PatchedVtable g_patched[4];
int g_patchedCount;

uintptr_t Original(const void* object, int slot)
{
    const auto* vtable = *static_cast<uintptr_t* const*>(object);
    for (int i = 0; i < g_patchedCount; i++)
        if (g_patched[i].vtable == vtable)
            return g_patched[i].orig[slot];
    return vtable[slot];
}

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*, const D3D10_BUFFER_DESC*, const D3D10_SUBRESOURCE_DATA*, ID3D10Buffer**);
using CreateTex1DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*, const D3D10_TEXTURE1D_DESC*, const D3D10_SUBRESOURCE_DATA*, ID3D10Texture1D**);
using CreateTex2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*, const D3D10_TEXTURE2D_DESC*, const D3D10_SUBRESOURCE_DATA*, ID3D10Texture2D**);
using CreateTex3DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*, const D3D10_TEXTURE3D_DESC*, const D3D10_SUBRESOURCE_DATA*, ID3D10Texture3D**);
using CreateQueryFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*, const D3D10_QUERY_DESC*, ID3D10Query**);
using BufferMapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Buffer*, D3D10_MAP, UINT, void**);
using Tex2DMapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Texture2D*, UINT, D3D10_MAP, UINT, D3D10_MAPPED_TEXTURE2D*);
BufferMapFn g_bufferMap;
Tex2DMapFn g_tex2DMap;

HRESULT STDMETHODCALLTYPE CensusCreateBuffer(ID3D10Device* d, const D3D10_BUFFER_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Buffer** out)
{
    InterlockedIncrement64(&g_deviceCalls[71]);
    if (desc)
    {
        char key[128];
        snprintf(key, sizeof(key), "Buffer usage=%u bind=0x%x cpu=0x%x misc=0x%x init=%d", desc->Usage, desc->BindFlags,
                 desc->CPUAccessFlags, desc->MiscFlags, init != nullptr);
        Note(g_created, key);
    }
    return reinterpret_cast<CreateBufferFn>(Original(d, 71))(d, desc, init, out);
}

HRESULT STDMETHODCALLTYPE CensusCreateTex1D(ID3D10Device* d, const D3D10_TEXTURE1D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture1D** out)
{
    InterlockedIncrement64(&g_deviceCalls[72]);
    if (desc)
    {
        char key[160];
        snprintf(key, sizeof(key), "Tex1D fmt=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x", desc->Format, desc->Usage, desc->BindFlags,
                 desc->CPUAccessFlags, desc->MiscFlags);
        Note(g_created, key);
    }
    return reinterpret_cast<CreateTex1DFn>(Original(d, 72))(d, desc, init, out);
}

HRESULT STDMETHODCALLTYPE CensusCreateTex2D(ID3D10Device* d, const D3D10_TEXTURE2D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture2D** out)
{
    InterlockedIncrement64(&g_deviceCalls[73]);
    if (desc)
    {
        char key[160];
        snprintf(key, sizeof(key), "Tex2D fmt=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x msaa=%u array=%u", desc->Format, desc->Usage,
                 desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags, desc->SampleDesc.Count, desc->ArraySize);
        Note(g_created, key);
    }
    return reinterpret_cast<CreateTex2DFn>(Original(d, 73))(d, desc, init, out);
}

HRESULT STDMETHODCALLTYPE CensusCreateTex3D(ID3D10Device* d, const D3D10_TEXTURE3D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture3D** out)
{
    InterlockedIncrement64(&g_deviceCalls[74]);
    if (desc)
    {
        char key[160];
        snprintf(key, sizeof(key), "Tex3D fmt=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x", desc->Format, desc->Usage, desc->BindFlags,
                 desc->CPUAccessFlags, desc->MiscFlags);
        Note(g_created, key);
    }
    return reinterpret_cast<CreateTex3DFn>(Original(d, 74))(d, desc, init, out);
}

HRESULT STDMETHODCALLTYPE CensusCreateQuery(ID3D10Device* d, const D3D10_QUERY_DESC* desc, ID3D10Query** out)
{
    InterlockedIncrement64(&g_deviceCalls[87]);
    if (desc)
    {
        char key[64];
        snprintf(key, sizeof(key), "Query type=%u misc=0x%x", desc->Query, desc->MiscFlags);
        Note(g_created, key);
    }
    return reinterpret_cast<CreateQueryFn>(Original(d, 87))(d, desc, out);
}

HRESULT STDMETHODCALLTYPE CensusBufferMap(ID3D10Buffer* b, D3D10_MAP type, UINT flags, void** data)
{
    D3D10_BUFFER_DESC desc{};
    b->GetDesc(&desc);
    char key[96];
    snprintf(key, sizeof(key), "Buffer map=%u flags=%u usage=%u bind=0x%x", type, flags, desc.Usage, desc.BindFlags);
    Note(g_maps, key);
    return g_bufferMap(b, type, flags, data);
}

HRESULT STDMETHODCALLTYPE CensusTex2DMap(ID3D10Texture2D* t, UINT sub, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE2D* data)
{
    D3D10_TEXTURE2D_DESC desc{};
    t->GetDesc(&desc);
    char key[96];
    snprintf(key, sizeof(key), "Tex2D map=%u flags=%u usage=%u fmt=%u", type, flags, desc.Usage, desc.Format);
    Note(g_maps, key);
    return g_tex2DMap(t, sub, type, flags, data);
}

// Counting stubs on every device slot, C++ hooks on creation and Map. An implementation vtable is shared by all its
// objects, so it is patched once, and one buffer and one texture are enough to reach Map.
void InstallCensus(ID3D10Device* device)
{
    auto* vtable = *reinterpret_cast<uintptr_t**>(device);
    for (int i = 0; i < g_patchedCount; i++)
        if (g_patched[i].vtable == vtable)
            return;
    if (g_patchedCount == 4)
        return;
    PatchedVtable& patched = g_patched[g_patchedCount];
    patched.vtable = vtable;
    memcpy(patched.orig, vtable, sizeof(patched.orig));
    g_patchedCount++;
    auto* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, kDeviceSlots * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stubs)
        return;
    const auto createBuffer = reinterpret_cast<CreateBufferFn>(patched.orig[71]);
    const auto createTex2D = reinterpret_cast<CreateTex2DFn>(patched.orig[73]);
    for (int s = 0; s < kDeviceSlots; s++)
    {
        uintptr_t target;
        switch (s)
        {
        case 71: target = reinterpret_cast<uintptr_t>(&CensusCreateBuffer); break;
        case 72: target = reinterpret_cast<uintptr_t>(&CensusCreateTex1D); break;
        case 73: target = reinterpret_cast<uintptr_t>(&CensusCreateTex2D); break;
        case 74: target = reinterpret_cast<uintptr_t>(&CensusCreateTex3D); break;
        case 87: target = reinterpret_cast<uintptr_t>(&CensusCreateQuery); break;
        default:
            target = reinterpret_cast<uintptr_t>(stubs + s * 32);
            EmitCountStub(stubs + s * 32, &g_deviceCalls[s], patched.orig[s]);
        }
        PatchSlot(vtable, s, target);
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, kDeviceSlots * 32);
    D3D10_BUFFER_DESC bd{16, D3D10_USAGE_DEFAULT, D3D10_BIND_VERTEX_BUFFER, 0, 0};
    ID3D10Buffer* buffer = nullptr;
    if (!g_bufferMap && SUCCEEDED(createBuffer(device, &bd, nullptr, &buffer)) && buffer)
    {
        auto* bvt = *reinterpret_cast<uintptr_t**>(buffer);
        g_bufferMap = reinterpret_cast<BufferMapFn>(bvt[kMapSlot]);
        PatchSlot(bvt, kMapSlot, reinterpret_cast<uintptr_t>(&CensusBufferMap));
        buffer->Release();
    }
    D3D10_TEXTURE2D_DESC td{};
    td.Width = td.Height = 4;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT;
    td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    ID3D10Texture2D* tex = nullptr;
    if (!g_tex2DMap && SUCCEEDED(createTex2D(device, &td, nullptr, &tex)) && tex)
    {
        auto* tvt = *reinterpret_cast<uintptr_t**>(tex);
        g_tex2DMap = reinterpret_cast<Tex2DMapFn>(tvt[kMapSlot]);
        PatchSlot(tvt, kMapSlot, reinterpret_cast<uintptr_t>(&CensusTex2DMap));
        tex->Release();
    }
    SetupLog("d3d census installed on the device");
}

// Stage A (CRYMT_D3DWRAP=1): the renderer gets our ID3D10Device1, which forwards every call to the real device and
// counts it. It is where the D3D12 back end will go. QueryInterface answers ID3D10Device/ID3D10Device1/IUnknown with
// the wrapper and forwards the rest (IDXGIDevice for the swap chain, ID3D10Multithread); the wrapper lives as long as
// the real device's references.
bool g_wrapOn;
bool g_d3d12On;                 // CRYMT_D3D12: the renderer's device on the D3D12 back end
ID3D10Device* g_d3d12Device;
uint64_t g_wrapCalls[101]; // plain increments: the device is single-threaded (render thread, or main when it is idle)

struct WrapDevice
{
    const void* const* vtbl;
    ID3D10Device1* real;
};

void StreamSync(int reason);
bool g_syncBefore[101]; // device slots that wait for the command stream before going to the real device

inline void Count(int slot)
{
    g_wrapCalls[slot]++;
    if (g_syncBefore[slot])
        StreamSync(slot);
}

inline ID3D10Device1* Real(ID3D10Device1* self)
{
    return reinterpret_cast<WrapDevice*>(self)->real;
}

inline void* RealSlot(ID3D10Device1* self, int slot)
{
    return (*reinterpret_cast<void* const* const*>(Real(self)))[slot];
}

ULONG STDMETHODCALLTYPE W_AddRef(ID3D10Device1* This)
{
    Count(1);
    return Real(This)->AddRef();
}

ULONG STDMETHODCALLTYPE W_Release(ID3D10Device1* This)
{
    Count(2);
    StreamSync(2);
    const ULONG refs = Real(This)->Release();
    if (refs == 0)
        delete reinterpret_cast<WrapDevice*>(This);
    return refs;
}

HRESULT STDMETHODCALLTYPE W_QueryInterface(ID3D10Device1* This, REFIID riid, void** out)
{
    Count(0);
    if (IsEqualIID(riid, __uuidof(ID3D10Device)) || IsEqualIID(riid, __uuidof(ID3D10Device1)) || IsEqualIID(riid, __uuidof(IUnknown)))
    {
        W_AddRef(This);
        *out = This;
        return S_OK;
    }
    if (g_censusOn)
    {
        const void* caller = __builtin_return_address(0);
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(caller), &module);
        wchar_t path[MAX_PATH] = L"?";
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* file = wcsrchr(path, L'\\');
        char key[160];
        snprintf(key, sizeof(key), "device QI {%08lx-%04x-%04x} from %ls+0x%llx", riid.Data1, riid.Data2, riid.Data3, file ? file + 1 : path,
                 static_cast<unsigned long long>(static_cast<const uint8_t*>(caller) - reinterpret_cast<const uint8_t*>(module)));
        Note(g_created, key);
    }
    return Real(This)->QueryInterface(riid, out);
}

#include "D3DWrapDevice.inc"

// ---------------------------------------------------------------------------------------------------------------------
// Stage B1 (CRYMT_D3DSTREAM=1, implies the wrapper): immediate-context calls of the renderer's device are recorded into
// a command stream that a stream thread executes on the real device, in order. B1a: state setters, draws, clears and
// copies are recorded (their objects AddRef'd until executed); every call whose effect depends on its order against
// them (resource Map, query Begin/End, Present and the swap chain's mode changes, getters, UpdateSubresource,
// SetPredication, SOSetTargets) first waits until the stream thread has executed everything recorded. Only the
// renderer's final device (the one it creates SINGLETHREADED) gets the stream, and it is created without that flag so
// the runtime locks around calls from the render thread (creation, Map, Present) beside the stream thread.
// ---------------------------------------------------------------------------------------------------------------------
bool g_streamOn;
constexpr size_t kStreamBytes = 64u << 20;
constexpr uint64_t kPublishBytes = 1024; // the producer hands commands over in batches of about this size
uint8_t* g_stream;
HANDLE g_streamWake;
ID3D10Device1* g_streamDevice; // the real device the stream thread calls; set once the stream runs
uint64_t g_streamCommands, g_streamPublishes, g_streamBusyTicks;

// Byte positions in the stream, monotonic. Each shared one has its own cache line: the producer and the stream thread
// touch them at different rates.
alignas(64) std::atomic<uint64_t> g_recordPos{0};   // producer: end of the last complete command
alignas(64) std::atomic<uint64_t> g_streamWrite{0}; // published to the stream thread
alignas(64) std::atomic<uint64_t> g_streamRead{0};  // executed by the stream thread
alignas(64) std::atomic<int> g_streamSleeping{0};
alignas(64) uint64_t g_readSeen;                    // producer: its last look at g_streamRead
uint64_t g_publishedSeen;                           // producer: what it last published itself

// Sync reasons: device slots 0..100, then the resource and swap chain methods.
enum SyncReason
{
    kSyncBufferMap = 101,
    kSyncTextureMap,
    kSyncAsyncBegin,
    kSyncAsyncEnd,
    kSyncPresent,
    kSyncSetFullscreen,
    kSyncResizeBuffers,
    kSyncResizeTarget,
    kSyncHeldFull,
    kSyncBufferNoOverwrite,
    kSyncTextureUnmap,
    kSyncAsyncGetData,
    kSyncReasons
};
const char* const kSyncReasonNames[kSyncReasons - 101] = {"Buffer::Map (read/write)",
                                                          "Texture::Map",
                                                          "Asynchronous::Begin",
                                                          "Asynchronous::End",
                                                          "SwapChain::Present",
                                                          "SwapChain::SetFullscreenState",
                                                          "SwapChain::ResizeBuffers",
                                                          "SwapChain::ResizeTarget",
                                                          "held objects list full",
                                                          "Buffer::Map NO_OVERWRITE (own uploads)",
                                                          "Texture::Unmap",
                                                          "Asynchronous::GetData"};
uint64_t g_syncs[kSyncReasons], g_syncWaits[kSyncReasons], g_syncWaitTicks[kSyncReasons];

struct CmdHeader
{
    uint32_t op;    // device slot, or one of the stream's own ops
    uint32_t bytes; // whole command, multiple of 8
};
constexpr uint32_t kOpUpload = 0x100; // buffer contents written through the data ring
constexpr uint32_t kOpScene = 0x101;  // the scene is complete (before HDR post-processing): DLSS on the D3D12 back end
constexpr uint32_t kOpDrawPacket = 0x102; // a draw with its own constants (DrawPacket.h), on the D3D12 back end
constexpr uint32_t kOpCamera = 0x103;     // the main camera was set: GI reprojection on the D3D12 back end
constexpr uint32_t kOpDrawGroup = 0x104;  // draws of one mesh range by a group of objects, constants computed here (ZPass)
constexpr uint32_t kOpPassName = 0x106;   // a pass name for frame debuggers begins (text) or ends (empty)
constexpr uint32_t kOpMatrices = 0x105;   // the renderer's modelview and projection changed: GI's shadow map geometry
constexpr uint32_t kOpUpdate = 0x107;     // UpdateSubresource with its source data copied into the data ring
constexpr uint32_t kOpPresent = 0x108;    // the swap chain's Present, made by the stream thread in order
constexpr uint32_t kOpDiscard = 0x109;    // a D3D12 dynamic buffer switches to the memory its producer Map(DISCARD) took
constexpr uint32_t kOpQueryBegin = 0x10A; // ID3D10Asynchronous::Begin, made by the stream thread in order (D3D12)
constexpr uint32_t kOpQueryEnd = 0x10B;   // ... End
constexpr uint32_t kOpQueryFlush = 0x10C; // submit the list a query ends in (a GetData asked for a flush)
constexpr uint32_t kOpTexUpload = 0x10D;  // a dynamic texture's Map(DISCARD) contents written through the data ring
using QueryBeginEndFn = void(STDMETHODCALLTYPE*)(ID3D10Asynchronous*);
using QueryGetDataFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Asynchronous*, void*, UINT, UINT);
QueryBeginEndFn g_queryBegin, g_queryEnd; // the back end's (StreamQueryBegin and below)
QueryGetDataFn g_queryGetData;
constexpr uint32_t kOpWrap = 0xFFFF;

// Recording is meant to come from one thread at a time (the render thread, or main while it is idle); thread changes
// are counted to check that.
DWORD g_recorderThread;
uint64_t g_recorderSwitches;

inline void NoteRecorder()
{
    const DWORD tid = GetCurrentThreadId();
    if (tid != g_recorderThread)
    {
        g_recorderThread = tid;
        g_recorderSwitches++;
    }
}

// Wakes the stream thread if it went to sleep; one wake per sleep.
void StreamKick()
{
    if (g_streamSleeping.load(std::memory_order_seq_cst) && g_streamSleeping.exchange(0, std::memory_order_seq_cst))
        SetEvent(g_streamWake);
}

// Hands every complete command to the stream thread. Any thread may publish (a sync from another thread while the
// producer is idle), so the published position only moves forward.
void StreamPublish()
{
    const uint64_t pos = g_recordPos.load(std::memory_order_acquire);
    uint64_t seen = g_streamWrite.load(std::memory_order_relaxed);
    while (seen < pos && !g_streamWrite.compare_exchange_weak(seen, pos, std::memory_order_seq_cst, std::memory_order_relaxed))
    {
    }
    g_streamPublishes++;
    StreamKick();
}

void StreamWaitSpace(uint64_t write, uint32_t bytes)
{
    while (write + bytes - g_readSeen > kStreamBytes)
    {
        g_readSeen = g_streamRead.load(std::memory_order_acquire);
        if (write + bytes - g_readSeen > kStreamBytes)
        {
            StreamPublish(); // the stream thread frees only what it was given
            _mm_pause();
        }
    }
}

// Space for one command in one piece; a command that would cross the end is preceded by a wrap filler.
uint8_t* StreamReserve(uint32_t bytes)
{
    uint64_t write = g_recordPos.load(std::memory_order_relaxed);
    const size_t pos = write % kStreamBytes;
    if (pos + bytes > kStreamBytes)
    {
        const auto pad = static_cast<uint32_t>(kStreamBytes - pos);
        StreamWaitSpace(write, pad);
        auto* h = reinterpret_cast<CmdHeader*>(g_stream + pos);
        h->op = kOpWrap;
        h->bytes = pad;
        write += pad;
        g_recordPos.store(write, std::memory_order_release);
    }
    StreamWaitSpace(write, bytes);
    return g_stream + write % kStreamBytes;
}

// Objects of recorded commands are AddRef'd by the producer and released by the producer after a sync has seen those
// commands executed, so their reference counts stay on one core. Each object is held once until then: an open-addressing
// set (kept at most half full) remembers which ones are, and the list keeps each one's slot for clearing.
struct HeldObject
{
    IUnknown* object;
    uint32_t slot;
};
constexpr size_t kHeldSlots = 1u << 15;
constexpr size_t kMaxHeld = kHeldSlots / 2;
const void* g_heldSet[kHeldSlots];
std::vector<HeldObject> g_held;
uint64_t g_heldTotal, g_holds;

void ReleaseHeld()
{
    for (const HeldObject& h : g_held)
    {
        g_heldSet[h.slot] = nullptr;
        h.object->Release();
    }
    g_heldTotal += g_held.size();
    g_held.clear();
}

// Waits until the stream thread has executed everything recorded so far.
void StreamSync(int reason)
{
    if (!g_streamDevice)
        return;
    g_syncs[reason]++;
    const uint64_t target = g_recordPos.load(std::memory_order_acquire);
    if (g_streamRead.load(std::memory_order_acquire) < target)
    {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        StreamPublish();
        while (g_streamRead.load(std::memory_order_acquire) < target)
            _mm_pause();
        QueryPerformanceCounter(&t1);
        g_syncWaits[reason]++;
        g_syncWaitTicks[reason] += t1.QuadPart - t0.QuadPart;
    }
    if (GetCurrentThreadId() == g_recorderThread)
        ReleaseHeld();
}

// Recording: a command is a header, then 8-byte argument words, then copied arrays.
struct Recorder
{
    uint8_t* base;
    uint32_t used;
    void Word(uint64_t v)
    {
        memcpy(base + used, &v, 8);
        used += 8;
    }
    void Bytes(const void* p, size_t n)
    {
        memcpy(base + used, p, n);
        used += static_cast<uint32_t>((n + 7) & ~size_t{7});
    }
};

// Trace of the recorded commands (op | size << 16) while CryMT's ZPass traces one z-prepass on the render thread.
std::vector<uint32_t>* g_trace;

Recorder Begin(int op, uint32_t bytes)
{
    NoteRecorder();
    if (g_trace)
        g_trace->push_back(static_cast<uint32_t>(op) | std::min<uint32_t>(bytes, 0xFFFF) << 16);
    if (g_held.size() >= kMaxHeld) // between commands, so no object of an unfinished command is released
        StreamSync(kSyncHeldFull);
    bytes = (bytes + sizeof(CmdHeader) + 7) & ~7u;
    Recorder r{StreamReserve(bytes), sizeof(CmdHeader)};
    auto* h = reinterpret_cast<CmdHeader*>(r.base);
    h->op = static_cast<uint32_t>(op);
    h->bytes = bytes;
    return r;
}

void End(const Recorder& r)
{
    const uint64_t pos = g_recordPos.load(std::memory_order_relaxed) + reinterpret_cast<const CmdHeader*>(r.base)->bytes;
    g_recordPos.store(pos, std::memory_order_release);
    g_streamCommands++;
    if (pos - g_publishedSeen >= kPublishBytes || g_streamSleeping.load(std::memory_order_relaxed))
    {
        g_publishedSeen = pos;
        StreamPublish();
    }
}

void Hold(const void* object)
{
    if (!object)
        return;
    g_holds++;
    size_t i = (reinterpret_cast<uintptr_t>(object) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - 15);
    for (; g_heldSet[i]; i = (i + 1) & (kHeldSlots - 1))
        if (g_heldSet[i] == object)
            return;
    g_heldSet[i] = object;
    auto* unknown = static_cast<IUnknown*>(const_cast<void*>(object));
    unknown->AddRef();
    g_held.push_back({unknown, static_cast<uint32_t>(i)});
}

// Pattern: (start, count, objects[count]) - constant buffers, shader resources, samplers of every stage.
UINT g_setEnd[kWrapSlots]; // highest start + count seen per Set* slot

template <int Slot>
void STDMETHODCALLTYPE R_SetArray(ID3D10Device1*, UINT start, UINT count, void* const* items)
{
    count = items ? count : 0;
    g_setEnd[Slot] = std::max(g_setEnd[Slot], start + count);
    Recorder r = Begin(Slot, 16 + count * 8);
    r.Word(start);
    r.Word(count);
    for (UINT i = 0; i < count; i++)
    {
        Hold(items[i]);
        r.Word(reinterpret_cast<uint64_t>(items[i]));
    }
    End(r);
}

// Pattern: one object - shaders, input layout, rasterizer state.
template <int Slot>
void STDMETHODCALLTYPE R_SetObject(ID3D10Device1*, void* object)
{
    Recorder r = Begin(Slot, 8);
    Hold(object);
    r.Word(reinterpret_cast<uint64_t>(object));
    End(r);
}

// Pattern: plain 32-bit arguments - draws, topology.
// Before a draw: the renderer's matrices when they changed and GI wants them (CryMtPassMatrices, render thread).
void NotePassMatrices()
{
    float m[32];
    if (!Dx12WantsPassMatrices() || !CryMtPassMatrices(m))
        return;
    Recorder r = Begin(kOpMatrices, sizeof(m));
    r.Bytes(m, sizeof(m));
    End(r);
}

template <int Slot>
void STDMETHODCALLTYPE R_Ints2(ID3D10Device1*, UINT a, UINT b)
{
    if constexpr (Slot == 9)
        NotePassMatrices();
    Recorder r = Begin(Slot, 16);
    r.Word(a);
    r.Word(b);
    End(r);
}

template <int Slot>
void STDMETHODCALLTYPE R_Ints3(ID3D10Device1*, UINT a, UINT b, INT c)
{
    if constexpr (Slot == 8)
        NotePassMatrices();
    Recorder r = Begin(Slot, 24);
    r.Word(a);
    r.Word(b);
    r.Word(static_cast<uint32_t>(c));
    End(r);
}

void STDMETHODCALLTYPE R_DrawIndexedInstanced(ID3D10Device1*, UINT a, UINT b, UINT c, INT d, UINT e)
{
    NotePassMatrices();
    Recorder r = Begin(14, 40);
    r.Word(a);
    r.Word(b);
    r.Word(c);
    r.Word(static_cast<uint32_t>(d));
    r.Word(e);
    End(r);
}

void STDMETHODCALLTYPE R_DrawInstanced(ID3D10Device1*, UINT a, UINT b, UINT c, UINT d)
{
    NotePassMatrices();
    Recorder r = Begin(15, 32);
    r.Word(a);
    r.Word(b);
    r.Word(c);
    r.Word(d);
    End(r);
}

void STDMETHODCALLTYPE R_IASetPrimitiveTopology(ID3D10Device1*, D3D10_PRIMITIVE_TOPOLOGY t)
{
    Recorder r = Begin(18, 8);
    r.Word(static_cast<uint32_t>(t));
    End(r);
}

void STDMETHODCALLTYPE R_IASetVertexBuffers(ID3D10Device1*, UINT start, UINT count, ID3D10Buffer* const* buffers, const UINT* strides, const UINT* offsets)
{
    Recorder r = Begin(12, 16 + count * 16);
    r.Word(start);
    r.Word(count);
    for (UINT i = 0; i < count; i++)
    {
        ID3D10Buffer* b = buffers ? buffers[i] : nullptr;
        Hold(b);
        r.Word(reinterpret_cast<uint64_t>(b));
        r.Word(static_cast<uint64_t>(strides ? strides[i] : 0) | static_cast<uint64_t>(offsets ? offsets[i] : 0) << 32);
    }
    End(r);
}

void STDMETHODCALLTYPE R_IASetIndexBuffer(ID3D10Device1*, ID3D10Buffer* buffer, DXGI_FORMAT format, UINT offset)
{
    Recorder r = Begin(13, 24);
    Hold(buffer);
    r.Word(reinterpret_cast<uint64_t>(buffer));
    r.Word(static_cast<uint32_t>(format));
    r.Word(offset);
    End(r);
}

void STDMETHODCALLTYPE R_OMSetRenderTargets(ID3D10Device1*, UINT count, ID3D10RenderTargetView* const* views, ID3D10DepthStencilView* depth)
{
    count = views ? count : 0;
    Recorder r = Begin(24, 16 + count * 8);
    r.Word(count);
    Hold(depth);
    r.Word(reinterpret_cast<uint64_t>(depth));
    for (UINT i = 0; i < count; i++)
    {
        Hold(views[i]);
        r.Word(reinterpret_cast<uint64_t>(views[i]));
    }
    End(r);
}

void STDMETHODCALLTYPE R_OMSetBlendState(ID3D10Device1*, ID3D10BlendState* state, const FLOAT factor[4], UINT mask)
{
    Recorder r = Begin(25, 40);
    Hold(state);
    r.Word(reinterpret_cast<uint64_t>(state));
    r.Word(factor != nullptr);
    const float zero[4] = {};
    r.Bytes(factor ? factor : zero, 16);
    r.Word(mask);
    End(r);
}

void STDMETHODCALLTYPE R_OMSetDepthStencilState(ID3D10Device1*, ID3D10DepthStencilState* state, UINT ref)
{
    Recorder r = Begin(26, 16);
    Hold(state);
    r.Word(reinterpret_cast<uint64_t>(state));
    r.Word(ref);
    End(r);
}

void STDMETHODCALLTYPE R_RSSetViewports(ID3D10Device1*, UINT count, const D3D10_VIEWPORT* viewports)
{
    count = viewports ? count : 0;
    Recorder r = Begin(30, 8 + count * sizeof(D3D10_VIEWPORT));
    r.Word(count);
    r.Bytes(viewports, count * sizeof(D3D10_VIEWPORT));
    End(r);
}

void STDMETHODCALLTYPE R_RSSetScissorRects(ID3D10Device1*, UINT count, const D3D10_RECT* rects)
{
    count = rects ? count : 0;
    Recorder r = Begin(31, 8 + count * sizeof(D3D10_RECT));
    r.Word(count);
    r.Bytes(rects, count * sizeof(D3D10_RECT));
    End(r);
}

void STDMETHODCALLTYPE R_CopySubresourceRegion(ID3D10Device1*, ID3D10Resource* dst, UINT dstSub, UINT x, UINT y, UINT z, ID3D10Resource* src, UINT srcSub,
                                               const D3D10_BOX* box)
{
    Recorder r = Begin(32, 64 + sizeof(D3D10_BOX));
    Hold(dst);
    Hold(src);
    r.Word(reinterpret_cast<uint64_t>(dst));
    r.Word(dstSub);
    r.Word(x);
    r.Word(y);
    r.Word(z);
    r.Word(reinterpret_cast<uint64_t>(src));
    r.Word(srcSub);
    r.Word(box != nullptr);
    const D3D10_BOX none{};
    r.Bytes(box ? box : &none, sizeof(D3D10_BOX));
    End(r);
}

void STDMETHODCALLTYPE R_CopyResource(ID3D10Device1*, ID3D10Resource* dst, ID3D10Resource* src)
{
    Recorder r = Begin(33, 16);
    Hold(dst);
    Hold(src);
    r.Word(reinterpret_cast<uint64_t>(dst));
    r.Word(reinterpret_cast<uint64_t>(src));
    End(r);
}

void STDMETHODCALLTYPE R_ClearRenderTargetView(ID3D10Device1*, ID3D10RenderTargetView* view, const FLOAT color[4])
{
    Recorder r = Begin(35, 24);
    Hold(view);
    r.Word(reinterpret_cast<uint64_t>(view));
    r.Bytes(color, 16);
    End(r);
}

void STDMETHODCALLTYPE R_ClearDepthStencilView(ID3D10Device1*, ID3D10DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil)
{
    Recorder r = Begin(36, 32);
    Hold(view);
    r.Word(reinterpret_cast<uint64_t>(view));
    r.Word(flags);
    uint32_t d;
    memcpy(&d, &depth, 4);
    r.Word(d);
    r.Word(stencil);
    End(r);
}

void STDMETHODCALLTYPE R_GenerateMips(ID3D10Device1*, ID3D10ShaderResourceView* view)
{
    Recorder r = Begin(37, 8);
    Hold(view);
    r.Word(reinterpret_cast<uint64_t>(view));
    End(r);
}

void STDMETHODCALLTYPE R_ResolveSubresource(ID3D10Device1*, ID3D10Resource* dst, UINT dstSub, ID3D10Resource* src, UINT srcSub, DXGI_FORMAT format)
{
    Recorder r = Begin(38, 40);
    Hold(dst);
    Hold(src);
    r.Word(reinterpret_cast<uint64_t>(dst));
    r.Word(dstSub);
    r.Word(reinterpret_cast<uint64_t>(src));
    r.Word(srcSub);
    r.Word(static_cast<uint32_t>(format));
    End(r);
}

template <int Slot>
void STDMETHODCALLTYPE R_NoArgs(ID3D10Device1*)
{
    Recorder r = Begin(Slot, 0);
    End(r);
}

// B1b: a buffer's Map(WRITE_DISCARD) returns memory from the data ring at once; its Unmap records an upload that the
// stream thread performs with a real Map(WRITE_DISCARD) in stream order. Other map types sync and map the real buffer.
// The ring is freed in stream order: an upload frees everything before the oldest buffer still mapped when it was
// recorded (nested maps may be unmapped in any order).
constexpr size_t kDataBytes = 32u << 20;
uint8_t* g_data;
uint64_t g_dataWrite;                             // producer side
uint64_t g_dataReadSeen;                          // producer: its last look at g_dataRead
alignas(64) std::atomic<uint64_t> g_dataRead{0}; // stream side: everything before is free
struct RingMap
{
    ID3D10Buffer* buffer; // or the texture of a texture map (texture set)
    uint64_t start;
    uint32_t size;
    uint32_t sub = 0, rowBytes = 0, rowPitch = 0, rows = 0, slices = 0, depthPitch = 0;
    bool texture = false;
};
constexpr int kMaxRingMaps = 16;
RingMap g_ringMaps[kMaxRingMaps]; // mapped now, in Map order
int g_ringMapCount;
uint64_t g_ringUploads, g_ringBytes;

using StreamBufferMapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Buffer*, D3D10_MAP, UINT, void**);
using StreamBufferUnmapFn = void(STDMETHODCALLTYPE*)(ID3D10Buffer*);
struct BufferVtable
{
    const void* vtable;
    StreamBufferMapFn map;
    StreamBufferUnmapFn unmap;
};
constexpr int kMaxBufferVtables = 4;
BufferVtable g_bufferVtables[kMaxBufferVtables];
std::atomic<int> g_bufferVtableCount{0};

// Stream position after the last upload recorded for a buffer. Map(NO_OVERWRITE) promises not to touch data that
// pending draws use, so it maps the real buffer at once from the producer, after waiting only for the buffer's own
// pending upload (its DISCARD renames the buffer). Entries are never removed: a stale one is already executed.
constexpr size_t kUploadSlots = 8192;
struct UploadPos
{
    ID3D10Buffer* buffer;
    uint64_t pos;
};
UploadPos g_uploadPos[kUploadSlots];

inline size_t UploadSlot(ID3D10Buffer* buffer)
{
    return (reinterpret_cast<uintptr_t>(buffer) >> 4) * 0x9E3779B97F4A7C15ull >> 51; // 13 bits
}

// An upload the table has no room for applies to every buffer.
uint64_t g_uploadAnyPos;

// Slots are only ever overwritten, never emptied, so probe chains stay intact; the first entry for a buffer along its
// chain is its newest.
void NoteUpload(ID3D10Buffer* buffer, uint64_t pos)
{
    for (size_t i = UploadSlot(buffer), n = 0; n < 32; i = (i + 1) % kUploadSlots, n++)
    {
        if (g_uploadPos[i].buffer == buffer || g_uploadPos[i].pos <= g_readSeen)
        {
            g_uploadPos[i] = {buffer, pos};
            return;
        }
    }
    g_uploadAnyPos = pos;
}

uint64_t LastUpload(ID3D10Buffer* buffer)
{
    for (size_t i = UploadSlot(buffer), n = 0; n < 32; i = (i + 1) % kUploadSlots, n++)
        if (g_uploadPos[i].buffer == buffer)
            return std::max(g_uploadPos[i].pos, g_uploadAnyPos);
    return g_uploadAnyPos;
}

// Waits until the stream thread has executed up to pos (a partial sync).
void StreamWaitFor(uint64_t pos, int reason)
{
    g_syncs[reason]++;
    if (g_readSeen >= pos)
        return;
    g_readSeen = g_streamRead.load(std::memory_order_acquire);
    if (g_readSeen >= pos)
        return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    StreamPublish();
    while ((g_readSeen = g_streamRead.load(std::memory_order_acquire)) < pos)
        _mm_pause();
    QueryPerformanceCounter(&t1);
    g_syncWaits[reason]++;
    g_syncWaitTicks[reason] += t1.QuadPart - t0.QuadPart;
}

const BufferVtable& BufferOriginals(ID3D10Buffer* buffer)
{
    const void* vtable = *reinterpret_cast<void* const*>(buffer);
    const int count = g_bufferVtableCount.load(std::memory_order_acquire);
    for (int i = 0; i < count - 1; i++)
        if (g_bufferVtables[i].vtable == vtable)
            return g_bufferVtables[i];
    return g_bufferVtables[count - 1];
}

// Direct maps on the D3D12 back end (CRYMT_DIRECTMAPS=0: the data ring as before). Map(DISCARD) takes the buffer's new
// upload memory on the producer, like a D3D11 driver, and records the switch; Map(NO_OVERWRITE) writes the producer's
// current memory at once. Before, NO_OVERWRITE waited for the stream thread to perform the buffer's last ring upload
// (harbor with GI: 2.3 ms a frame), and every DISCARD was copied twice (ring, then upload memory).
bool g_directMaps;
uint64_t g_directNoOverwrite;

HRESULT STDMETHODCALLTYPE StreamBufferMap(ID3D10Buffer* buffer, D3D10_MAP type, UINT flags, void** data)
{
    if (g_directMaps && data)
    {
        if (type == D3D10_MAP_WRITE_DISCARD)
        {
            Dx12Discard dd;
            if (Dx12DiscardNow(buffer, &dd))
            {
                *data = dd.cpu;
                if (!dd.cpu)
                    return E_OUTOFMEMORY;
                Recorder r = Begin(kOpDiscard, 48);
                Hold(buffer);
                r.Word(reinterpret_cast<uint64_t>(buffer));
                for (int k = 0; k < 5; k++)
                    r.Word(dd.words[k]);
                End(r);
                return S_OK;
            }
        }
        else if (type == D3D10_MAP_WRITE_NO_OVERWRITE)
        {
            if (void* p = Dx12ProducerMemory(buffer))
            {
                NoteRecorder();
                g_directNoOverwrite++;
                *data = p;
                return S_OK;
            }
        }
    }
    if (type == D3D10_MAP_WRITE_DISCARD && data && g_ringMapCount < kMaxRingMaps)
    {
        NoteRecorder();
        D3D10_BUFFER_DESC desc;
        buffer->GetDesc(&desc);
        const uint32_t size = (desc.ByteWidth + 15) & ~15u;
        uint64_t start = g_dataWrite;
        if (start % kDataBytes + size > kDataBytes)
            start += kDataBytes - start % kDataBytes;
        while (start + size - g_dataReadSeen > kDataBytes)
        {
            g_dataReadSeen = g_dataRead.load(std::memory_order_acquire);
            if (start + size - g_dataReadSeen > kDataBytes)
            {
                StreamPublish();
                _mm_pause();
            }
        }
        g_dataWrite = start + size;
        g_ringMaps[g_ringMapCount++] = {buffer, start, desc.ByteWidth};
        *data = g_data + start % kDataBytes;
        return S_OK;
    }
    if (type == D3D10_MAP_WRITE_NO_OVERWRITE)
        StreamWaitFor(LastUpload(buffer), kSyncBufferNoOverwrite);
    else
        StreamSync(kSyncBufferMap);
    return BufferOriginals(buffer).map(buffer, type, flags, data);
}

void STDMETHODCALLTYPE StreamBufferUnmap(ID3D10Buffer* buffer)
{
    for (int i = 0; i < g_ringMapCount; i++)
    {
        if (g_ringMaps[i].buffer != buffer)
            continue;
        const RingMap m = g_ringMaps[i];
        memmove(&g_ringMaps[i], &g_ringMaps[i + 1], (g_ringMapCount - i - 1) * sizeof(RingMap));
        g_ringMapCount--;
        const uint64_t freeTo = g_ringMapCount ? g_ringMaps[0].start : g_dataWrite;
        Recorder r = Begin(kOpUpload, 32);
        Hold(buffer);
        r.Word(reinterpret_cast<uint64_t>(buffer));
        r.Word(m.start);
        r.Word(m.size);
        r.Word(freeTo);
        End(r);
        NoteUpload(buffer, g_recordPos.load(std::memory_order_relaxed));
        g_ringUploads++;
        g_ringBytes += m.size;
        return;
    }
    BufferOriginals(buffer).unmap(buffer);
}

// UpdateSubresource without a sync: it waited for the whole stream before (the stream thread may sit in Present waiting
// for the GPU): harbor 0.1 waits a frame of ~16 ms each, 1.66 ms a frame on average and a hitch every ~10 frames. The
// source data the call reads (box, pitches, format) is copied into the data ring now; the stream thread makes the call
// in order. Formats it cannot size, and more than 8 MB, still sync.
bool FormatElement(DXGI_FORMAT f, UINT& bytes, bool& block)
{
    const int v = f;
    block = (v >= 70 && v <= 84) || (v >= 94 && v <= 99);
    if (block)
    {
        bytes = (v >= 70 && v <= 72) || (v >= 79 && v <= 81) ? 8 : 16;
        return true;
    }
    bytes = v >= 1 && v <= 4 ? 16 : v >= 5 && v <= 8 ? 12 : v >= 9 && v <= 22 ? 8 : (v >= 23 && v <= 47) || v == 67 || (v >= 87 && v <= 93) ? 4
          : (v >= 48 && v <= 59) || v == 85 || v == 86 ? 2 : v >= 60 && v <= 65 ? 1 : 0;
    return bytes != 0;
}

size_t UpdateSourceBytes(ID3D10Resource* dst, UINT sub, const D3D10_BOX* box, UINT rowPitch, UINT depthPitch)
{
    D3D10_RESOURCE_DIMENSION dim = D3D10_RESOURCE_DIMENSION_UNKNOWN;
    dst->GetType(&dim);
    if (dim == D3D10_RESOURCE_DIMENSION_BUFFER)
    {
        D3D10_BUFFER_DESC b{};
        static_cast<ID3D10Buffer*>(dst)->GetDesc(&b);
        return box ? (box->right > box->left ? box->right - box->left : 0) : b.ByteWidth;
    }
    UINT w = 1, h = 1, depth = 1, mips = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
    {
        D3D10_TEXTURE1D_DESC t{};
        static_cast<ID3D10Texture1D*>(dst)->GetDesc(&t);
        w = t.Width, mips = t.MipLevels, format = t.Format;
    }
    else if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D10_TEXTURE2D_DESC t{};
        static_cast<ID3D10Texture2D*>(dst)->GetDesc(&t);
        w = t.Width, h = t.Height, mips = t.MipLevels, format = t.Format;
    }
    else if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D)
    {
        D3D10_TEXTURE3D_DESC t{};
        static_cast<ID3D10Texture3D*>(dst)->GetDesc(&t);
        w = t.Width, h = t.Height, depth = t.Depth, mips = t.MipLevels, format = t.Format;
    }
    else
        return SIZE_MAX;
    const UINT mip = mips ? sub % mips : 0;
    w = std::max(1u, w >> mip), h = std::max(1u, h >> mip), depth = std::max(1u, depth >> mip);
    if (box)
    {
        w = box->right > box->left ? box->right - box->left : 0;
        h = box->bottom > box->top ? box->bottom - box->top : 0;
        depth = box->back > box->front ? box->back - box->front : 0;
    }
    if (!w || !h || !depth)
        return 0;
    UINT element = 0;
    bool block = false;
    if (!FormatElement(format, element, block))
        return SIZE_MAX;
    const size_t rows = block ? (h + 3) / 4 : h, rowBytes = static_cast<size_t>(block ? (w + 3) / 4 : w) * element;
    if (rows > 1 && rowPitch < rowBytes)
        return SIZE_MAX;
    return static_cast<size_t>(depthPitch) * (depth - 1) + static_cast<size_t>(rowPitch) * (rows - 1) + rowBytes;
}

uint64_t g_updatesAsync, g_updatesSynced;

void STDMETHODCALLTYPE R_UpdateSubresource(ID3D10Device1* self, ID3D10Resource* dst, UINT sub, const D3D10_BOX* box, const void* data, UINT rowPitch,
                                           UINT depthPitch)
{
    const size_t bytes = dst && data ? UpdateSourceBytes(dst, sub, box, rowPitch, depthPitch) : SIZE_MAX;
    if (bytes == SIZE_MAX || bytes > (8u << 20))
    {
        g_updatesSynced++;
        using Fn = void(STDMETHODCALLTYPE*)(ID3D10Device1*, ID3D10Resource*, UINT, const D3D10_BOX*, const void*, UINT, UINT);
        reinterpret_cast<Fn>(const_cast<void*>(static_cast<const void*>(kWrapVtable[34])))(self, dst, sub, box, data, rowPitch, depthPitch); // counts, syncs, calls
        return;
    }
    g_wrapCalls[34]++;
    g_updatesAsync++;
    NoteRecorder();
    const uint32_t size = static_cast<uint32_t>((bytes + 15) & ~size_t{15});
    uint64_t start = g_dataWrite;
    if (size)
    {
        if (start % kDataBytes + size > kDataBytes)
            start += kDataBytes - start % kDataBytes;
        while (start + size - g_dataReadSeen > kDataBytes)
        {
            g_dataReadSeen = g_dataRead.load(std::memory_order_acquire);
            if (start + size - g_dataReadSeen > kDataBytes)
            {
                StreamPublish();
                _mm_pause();
            }
        }
        g_dataWrite = start + size;
        memcpy(g_data + start % kDataBytes, data, bytes);
    }
    const uint64_t freeTo = g_ringMapCount ? g_ringMaps[0].start : g_dataWrite;
    Recorder r = Begin(kOpUpdate, 48 + sizeof(D3D10_BOX));
    Hold(dst);
    r.Word(reinterpret_cast<uint64_t>(dst));
    r.Word(sub);
    r.Word(box != nullptr);
    r.Word(start);
    r.Word(rowPitch | static_cast<uint64_t>(depthPitch) << 32);
    r.Word(freeTo);
    const D3D10_BOX none{};
    r.Bytes(box ? box : &none, sizeof(D3D10_BOX));
    End(r);
}

void ExecuteUpdate(const uint64_t* w)
{
    using Fn = void(STDMETHODCALLTYPE*)(ID3D10Device1*, ID3D10Resource*, UINT, const D3D10_BOX*, const void*, UINT, UINT);
    const auto* box = w[2] ? reinterpret_cast<const D3D10_BOX*>(w + 6) : nullptr;
    const Fn fn = reinterpret_cast<Fn>((*reinterpret_cast<void* const* const*>(g_streamDevice))[34]);
    fn(g_streamDevice, reinterpret_cast<ID3D10Resource*>(w[0]), static_cast<UINT>(w[1]), box, g_data + w[3] % kDataBytes, static_cast<UINT>(w[4]),
       static_cast<UINT>(w[4] >> 32));
    g_dataRead.store(w[5], std::memory_order_release);
}

void ExecuteUpload(const uint64_t* w)
{
    auto* buffer = reinterpret_cast<ID3D10Buffer*>(w[0]);
    const BufferVtable& o = BufferOriginals(buffer);
    void* dst = nullptr;
    if (SUCCEEDED(o.map(buffer, D3D10_MAP_WRITE_DISCARD, 0, &dst)) && dst)
    {
        memcpy(dst, g_data + w[1] % kDataBytes, static_cast<size_t>(w[2]));
        o.unmap(buffer);
    }
    g_dataRead.store(w[3], std::memory_order_release);
}

void ExecutePresent(const uint64_t* w); // below, with the swap chain hooks
void ExecuteTexUpload(const uint64_t* w); // below, with the texture hooks

// Execution on the stream thread: calls the real device's current vtable.
template <typename Fn>
Fn Slot(int slot)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void* const* const*>(g_streamDevice))[slot]);
}

void Execute(const CmdHeader* h)
{
    const auto* w = reinterpret_cast<const uint64_t*>(h + 1);
    ID3D10Device1* d = g_streamDevice;
    const int op = static_cast<int>(h->op);
    auto obj = [&](int i) { return reinterpret_cast<void*>(w[i]); };
    switch (op)
    {
    case 3: case 4: case 6: case 10: case 16: case 19: case 20: case 22: case 23:
    {
        const UINT count = static_cast<UINT>(w[1]);
        void* items[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
        for (UINT i = 0; i < count && i < D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++)
            items[i] = obj(2 + i);
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, UINT, void* const*)>(op)(d, static_cast<UINT>(w[0]), count, count ? items : nullptr);
        break;
    }
    case 5: case 7: case 11: case 17: case 29:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*)>(op)(d, obj(0));
        break;
    case 9:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, UINT)>(op)(d, static_cast<UINT>(w[0]), static_cast<UINT>(w[1]));
        break;
    case 8:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, UINT, INT)>(op)(d, static_cast<UINT>(w[0]), static_cast<UINT>(w[1]), static_cast<INT>(w[2]));
        break;
    case 14:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, UINT, UINT, INT, UINT)>(op)(d, static_cast<UINT>(w[0]), static_cast<UINT>(w[1]),
                                                                                       static_cast<UINT>(w[2]), static_cast<INT>(w[3]), static_cast<UINT>(w[4]));
        break;
    case 15:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, UINT, UINT, UINT)>(op)(d, static_cast<UINT>(w[0]), static_cast<UINT>(w[1]),
                                                                                 static_cast<UINT>(w[2]), static_cast<UINT>(w[3]));
        break;
    case 18:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, D3D10_PRIMITIVE_TOPOLOGY)>(op)(d, static_cast<D3D10_PRIMITIVE_TOPOLOGY>(w[0]));
        break;
    case 12:
    {
        const UINT count = static_cast<UINT>(w[1]);
        ID3D10Buffer* buffers[16];
        UINT strides[16], offsets[16];
        for (UINT i = 0; i < count && i < 16; i++)
        {
            buffers[i] = static_cast<ID3D10Buffer*>(obj(2 + 2 * i));
            strides[i] = static_cast<UINT>(w[3 + 2 * i]);
            offsets[i] = static_cast<UINT>(w[3 + 2 * i] >> 32);
        }
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, UINT, ID3D10Buffer* const*, const UINT*, const UINT*)>(op)(d, static_cast<UINT>(w[0]), count, buffers,
                                                                                                                    strides, offsets);
        break;
    }
    case 13:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, DXGI_FORMAT, UINT)>(op)(d, obj(0), static_cast<DXGI_FORMAT>(w[1]), static_cast<UINT>(w[2]));
        break;
    case 24:
    {
        const UINT count = static_cast<UINT>(w[0]);
        void* views[8];
        for (UINT i = 0; i < count && i < 8; i++)
            views[i] = obj(2 + i);
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, void* const*, void*)>(op)(d, count, count ? views : nullptr, obj(1));
        break;
    }
    case 25:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, const FLOAT*, UINT)>(op)(d, obj(0), w[1] ? reinterpret_cast<const FLOAT*>(&w[2]) : nullptr,
                                                                                     static_cast<UINT>(w[4]));
        break;
    case 26:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, UINT)>(op)(d, obj(0), static_cast<UINT>(w[1]));
        break;
    case 30:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, const D3D10_VIEWPORT*)>(op)(d, static_cast<UINT>(w[0]), reinterpret_cast<const D3D10_VIEWPORT*>(&w[1]));
        break;
    case 31:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, UINT, const D3D10_RECT*)>(op)(d, static_cast<UINT>(w[0]), reinterpret_cast<const D3D10_RECT*>(&w[1]));
        break;
    case 32:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, UINT, UINT, UINT, UINT, void*, UINT, const D3D10_BOX*)>(op)(
            d, obj(0), static_cast<UINT>(w[1]), static_cast<UINT>(w[2]), static_cast<UINT>(w[3]), static_cast<UINT>(w[4]), obj(5), static_cast<UINT>(w[6]),
            w[7] ? reinterpret_cast<const D3D10_BOX*>(&w[8]) : nullptr);
        break;
    case 33:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, void*)>(op)(d, obj(0), obj(1));
        break;
    case 35:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, const FLOAT*)>(op)(d, obj(0), reinterpret_cast<const FLOAT*>(&w[1]));
        break;
    case 36:
    {
        float depth;
        const uint32_t bits = static_cast<uint32_t>(w[2]);
        memcpy(&depth, &bits, 4);
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, UINT, FLOAT, UINT8)>(op)(d, obj(0), static_cast<UINT>(w[1]), depth, static_cast<UINT8>(w[3]));
        break;
    }
    case 37:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*)>(op)(d, obj(0));
        break;
    case 38:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*, void*, UINT, void*, UINT, DXGI_FORMAT)>(op)(d, obj(0), static_cast<UINT>(w[1]), obj(2),
                                                                                                  static_cast<UINT>(w[3]), static_cast<DXGI_FORMAT>(w[4]));
        break;
    case 28: case 69: case 70:
        Slot<void(STDMETHODCALLTYPE*)(ID3D10Device1*)>(op)(d);
        break;
    case kOpUpload:
        ExecuteUpload(w);
        break;
    case kOpUpdate:
        ExecuteUpdate(w);
        break;
    case kOpPresent:
        ExecutePresent(w);
        break;
    case kOpDiscard:
        Dx12ApplyDiscard(reinterpret_cast<ID3D10Buffer*>(w[0]), w + 1);
        break;
    case kOpQueryBegin:
        g_queryBegin(reinterpret_cast<ID3D10Asynchronous*>(w[0]));
        break;
    case kOpQueryEnd:
        g_queryEnd(reinterpret_cast<ID3D10Asynchronous*>(w[0]));
        break;
    case kOpQueryFlush:
        Dx12QueryFlush(w[0]);
        break;
    case kOpTexUpload:
        ExecuteTexUpload(w);
        break;
    case kOpScene:
        Dx12SceneDone(reinterpret_cast<const DlssSceneInfo*>(w));
        break;
    case kOpCamera:
        Dx12Camera(reinterpret_cast<const DlssSceneInfo*>(w));
        break;
    case kOpMatrices:
        Dx12PassMatrices(reinterpret_cast<const float*>(w));
        break;
    case kOpPassName:
        Dx12PassName(reinterpret_cast<const char*>(w));
        break;
    case kOpDrawPacket:
        Dx12DrawPacket(reinterpret_cast<const uint8_t*>(w));
        break;
    case kOpDrawGroup:
        ZPassExpandGroup(reinterpret_cast<const uint8_t*>(w));
        break;
    default:
        break;
    }
}

// Runs commands as they come; spins briefly when the stream is empty, then sleeps until kicked. Busy time is measured
// per run of work, not per command.
DWORD WINAPI StreamMain(void*)
{
    LARGE_INTEGER runStart{};
    bool running = false;
    for (;;)
    {
        const uint64_t read = g_streamRead.load(std::memory_order_relaxed);
        if (g_streamWrite.load(std::memory_order_acquire) == read)
        {
            if (running)
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                g_streamBusyTicks += now.QuadPart - runStart.QuadPart;
                running = false;
            }
            bool idle = true;
            for (int spin = 0; spin < 4000 && idle; spin++)
            {
                _mm_pause();
                idle = g_streamWrite.load(std::memory_order_acquire) == read;
            }
            if (idle)
            {
                g_streamSleeping.store(1, std::memory_order_seq_cst);
                if (g_streamWrite.load(std::memory_order_seq_cst) == read)
                    WaitForSingleObject(g_streamWake, 2);
                g_streamSleeping.store(0, std::memory_order_relaxed);
            }
            continue;
        }
        if (!running)
        {
            QueryPerformanceCounter(&runStart);
            running = true;
        }
        // Everything published so far, with the read position stored every few commands and at the end.
        const uint64_t avail = g_streamWrite.load(std::memory_order_acquire);
        uint64_t pos = read;
        for (int n = 1; pos < avail; n++)
        {
            const auto* h = reinterpret_cast<const CmdHeader*>(g_stream + pos % kStreamBytes);
            if (h->op != kOpWrap)
                Execute(h);
            pos += h->bytes;
            if (n % 32 == 0)
                g_streamRead.store(pos, std::memory_order_release);
        }
        g_streamRead.store(pos, std::memory_order_release);
    }
}

// Sync stubs for methods of objects the wrapper does not own (resources, queries, the swap chain): they keep the
// argument registers, call StreamSync(reason), then jump to the original method, so one stub fits any signature
// without float arguments. A stub is made per patched (vtable, slot):
//   push rcx; push rdx; push r8; push r9; sub rsp, 28h; mov ecx, reason; mov rax, StreamSync; call rax;
//   add rsp, 28h; pop r9; pop r8; pop rdx; pop rcx; mov rax, original; jmp rax
constexpr size_t kSyncStubBytes = 64;
constexpr int kSyncStubs = 64;
uint8_t* g_syncStubs;
int g_syncStubCount;

const void* EmitSyncStub(int reason, uintptr_t original)
{
    if (!g_syncStubs || g_syncStubCount == kSyncStubs)
        return nullptr;
    uint8_t* const start = g_syncStubs + g_syncStubCount++ * kSyncStubBytes;
    uint8_t* p = start;
    const auto sync = reinterpret_cast<uint64_t>(static_cast<void (*)(int)>(&StreamSync));
    for (uint8_t b : {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x28, 0xB9})
        *p++ = b;
    memcpy(p, &reason, 4);
    p += 4;
    *p++ = 0x48; *p++ = 0xB8; memcpy(p, &sync, 8); p += 8;
    for (uint8_t b : {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59})
        *p++ = b;
    *p++ = 0x48; *p++ = 0xB8; memcpy(p, &original, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), start, kSyncStubBytes);
    return start;
}

// Objects whose methods must sync, by kind; an implementation vtable is shared by its objects, so each is patched once.
enum HookKind
{
    kHookBuffer,
    kHookTexture,
    kHookAsync,
    kHookSwapChain,
};
struct SyncSlot
{
    int slot, reason;
};
const SyncSlot kBufferSyncs[] = {{10, kSyncBufferMap}};
// Unmap and GetData sync too: on the D3D12 back end they record into or submit the command list the stream thread uses.
const SyncSlot kTextureSyncs[] = {{10, kSyncTextureMap}, {11, kSyncTextureUnmap}};
const SyncSlot kAsyncSyncs[] = {{7, kSyncAsyncBegin}, {8, kSyncAsyncEnd}, {9, kSyncAsyncGetData}};
const SyncSlot kSwapChainSyncs[] = {{8, kSyncPresent}, {10, kSyncSetFullscreen}, {13, kSyncResizeBuffers}, {14, kSyncResizeTarget}};

// Present without a full sync (CRYMT_ASYNCPRESENT=0: synced as before). It synced the whole stream and presented on the
// render thread, the stream thread idle meanwhile: harbor 1.9 ms a frame of the render thread waiting there once
// UpdateSubresource no longer synced. Now the render thread records the present and starts the next frame; the stream
// thread presents in order (on the D3D12 back end PresentFrame - the frame's submit, the swap chain - is its work anyway).
// At most one present is pending: the next one waits until the previous has been made. The result returned is the last
// one made (a lost device shows one frame later).
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_presentOriginal;
const void* g_presentVtable;
bool g_asyncPresent = true;
uint64_t g_lastPresentPos, g_presentsAsync;
std::atomic<HRESULT> g_presentResult{S_OK};

HRESULT STDMETHODCALLTYPE StreamPresent(IDXGISwapChain* self, UINT sync, UINT flags)
{
    if (!g_asyncPresent || (flags & DXGI_PRESENT_TEST) || *reinterpret_cast<const void* const*>(self) != g_presentVtable)
    {
        StreamSync(kSyncPresent);
        return g_presentOriginal(self, sync, flags);
    }
    StreamWaitFor(g_lastPresentPos, kSyncPresent);
    NoteRecorder();
    Recorder r = Begin(kOpPresent, 24);
    Hold(self);
    r.Word(reinterpret_cast<uint64_t>(self));
    r.Word(sync);
    r.Word(flags);
    End(r);
    g_lastPresentPos = g_recordPos.load(std::memory_order_relaxed);
    g_presentsAsync++;
    StreamPublish();
    return g_presentResult.load(std::memory_order_acquire);
}

void ExecutePresent(const uint64_t* w)
{
    const HRESULT hr = g_presentOriginal(reinterpret_cast<IDXGISwapChain*>(w[0]), static_cast<UINT>(w[1]), static_cast<UINT>(w[2]));
    g_presentResult.store(hr == DXGI_STATUS_OCCLUDED ? S_OK : hr, std::memory_order_release);
}

// Asynchronous queries on the D3D12 back end (CRYMT_ASYNCQUERIES=0: synced as before). Begin, End and GetData synced the
// whole stream: harbor with GI 1.8 GetData waits a frame (2.6 ms; 24 k polls a frame) and 0.6 Begin waits (1.0 ms), the
// render thread waiting for the stream thread to execute everything recorded, the frame's later work too. Now Begin and
// End are recorded; GetData answers S_FALSE (not ready, as D3D10 allows) until the stream thread has executed the
// query's own End, then asks the back end without a sync (Dx12QueryPoll); a flush it needs is recorded once per list.
const void* g_queryVtable;
bool g_asyncQueries;
uint64_t g_queryPolls, g_queryBehind, g_queryFlushes, g_queryFlushSerial;

constexpr size_t kQueryEndSlots = 1024;
struct QueryEndPos
{
    ID3D10Asynchronous* query;
    uint64_t pos;
};
QueryEndPos g_queryEnds[kQueryEndSlots];

uint64_t* QueryEndSlot(ID3D10Asynchronous* query, bool create)
{
    size_t i = (reinterpret_cast<uintptr_t>(query) >> 4) * 0x9E3779B97F4A7C15ull >> 54; // 10 bits
    for (size_t n = 0; n < kQueryEndSlots; n++, i = (i + 1) % kQueryEndSlots)
    {
        if (g_queryEnds[i].query == query)
            return &g_queryEnds[i].pos;
        if (!g_queryEnds[i].query)
        {
            if (!create)
                return nullptr;
            g_queryEnds[i].query = query;
            return &g_queryEnds[i].pos;
        }
    }
    return nullptr;
}

void STDMETHODCALLTYPE StreamQueryBegin(ID3D10Asynchronous* query)
{
    Recorder r = Begin(kOpQueryBegin, 8);
    Hold(query);
    r.Word(reinterpret_cast<uint64_t>(query));
    End(r);
}

void STDMETHODCALLTYPE StreamQueryEnd(ID3D10Asynchronous* query)
{
    Recorder r = Begin(kOpQueryEnd, 8);
    Hold(query);
    r.Word(reinterpret_cast<uint64_t>(query));
    End(r);
    if (uint64_t* pos = QueryEndSlot(query, true))
        *pos = g_recordPos.load(std::memory_order_relaxed);
    else
        StreamSync(kSyncAsyncEnd); // table full (40 queries in Crysis): the old way for this one
}

// Texture::Map / Unmap on the D3D12 back end. Map(WRITE_DISCARD) of a dynamic texture returns data ring memory at once
// (our own row pitch) and its Unmap records the upload; the stream thread maps the texture for real, copies the rows and
// unmaps (CRYMT_ASYNCTEXMAP=0: synced as before). Both waits on harbor were such uploads, not read-backs: the ocean's FFT
// displacement (CREWaterOcean::vf15 -> FUN_38092090, 0.65 a frame, 3.1 ms of render-thread waiting with the queries
// asynchronous) and the water volumes' normals (CWaterVolume::vf3, 0.6 a frame, 0.6 ms). Other maps (staging reads)
// still sync; the census keeps the callers of those.
using TextureMapFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3D10_MAP, UINT, void*);
using TextureUnmapFn = void(STDMETHODCALLTYPE*)(void*, UINT);
struct TextureVtable
{
    const void* vtable;
    TextureMapFn map;
    TextureUnmapFn unmap;
};
TextureVtable g_textureVtables[4];
int g_textureVtableCount;
bool g_asyncTexMaps;
uint64_t g_texMapsAsync;

const TextureVtable* TextureOriginals(const void* texture)
{
    const void* vtable = *static_cast<void* const*>(texture);
    for (int i = 0; i < g_textureVtableCount; i++)
        if (g_textureVtables[i].vtable == vtable)
            return &g_textureVtables[i];
    return nullptr;
}

// Space in the data ring for a producer map (waits for the stream thread only when the ring is full).
uint64_t RingReserve(uint32_t size)
{
    uint64_t start = g_dataWrite;
    if (start % kDataBytes + size > kDataBytes)
        start += kDataBytes - start % kDataBytes;
    while (start + size - g_dataReadSeen > kDataBytes)
    {
        g_dataReadSeen = g_dataRead.load(std::memory_order_acquire);
        if (start + size - g_dataReadSeen > kDataBytes)
        {
            StreamPublish();
            _mm_pause();
        }
    }
    g_dataWrite = start + size;
    return start;
}

// Map(WRITE_DISCARD) of a dynamic texture subresource through the data ring; false: the old way.
bool TextureMapToRing(void* texture, UINT sub, void* out)
{
    auto* res = static_cast<ID3D10Resource*>(texture);
    D3D10_RESOURCE_DIMENSION dim = D3D10_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    UINT w = 1, h = 1, depth = 1, mips = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D10_USAGE usage = D3D10_USAGE_DEFAULT;
    if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
    {
        D3D10_TEXTURE1D_DESC t{};
        static_cast<ID3D10Texture1D*>(res)->GetDesc(&t);
        w = t.Width, mips = t.MipLevels, format = t.Format, usage = t.Usage;
    }
    else if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D10_TEXTURE2D_DESC t{};
        static_cast<ID3D10Texture2D*>(res)->GetDesc(&t);
        w = t.Width, h = t.Height, mips = t.MipLevels, format = t.Format, usage = t.Usage;
    }
    else if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D)
    {
        D3D10_TEXTURE3D_DESC t{};
        static_cast<ID3D10Texture3D*>(res)->GetDesc(&t);
        w = t.Width, h = t.Height, depth = t.Depth, mips = t.MipLevels, format = t.Format, usage = t.Usage;
    }
    else
        return false;
    UINT bytes = 0;
    bool block = false;
    if (usage != D3D10_USAGE_DYNAMIC || !FormatElement(format, bytes, block) || !mips)
        return false;
    const UINT level = sub % mips;
    w = std::max(1u, w >> level), h = std::max(1u, h >> level), depth = std::max(1u, depth >> level);
    RingMap m;
    m.buffer = reinterpret_cast<ID3D10Buffer*>(texture);
    m.texture = true;
    m.sub = sub;
    m.rowBytes = (block ? (w + 3) / 4 : w) * bytes;
    m.rowPitch = (m.rowBytes + 15) & ~15u;
    m.rows = block ? (h + 3) / 4 : h;
    m.slices = dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? depth : 1;
    m.depthPitch = m.rowPitch * m.rows;
    const uint64_t size64 = static_cast<uint64_t>(m.depthPitch) * m.slices;
    if (size64 > kDataBytes / 4)
        return false;
    m.size = static_cast<uint32_t>(size64);
    NoteRecorder();
    m.start = RingReserve(m.size);
    g_ringMaps[g_ringMapCount++] = m;
    uint8_t* p = g_data + m.start % kDataBytes;
    if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
        *static_cast<void**>(out) = p;
    else if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE2D)
        *static_cast<D3D10_MAPPED_TEXTURE2D*>(out) = {p, m.rowPitch};
    else
        *static_cast<D3D10_MAPPED_TEXTURE3D*>(out) = {p, m.rowPitch, m.depthPitch};
    g_texMapsAsync++;
    return true;
}

void STDMETHODCALLTYPE StreamTextureUnmap(void* texture, UINT sub)
{
    for (int i = 0; i < g_ringMapCount; i++)
    {
        const RingMap& e = g_ringMaps[i];
        if (!e.texture || e.buffer != reinterpret_cast<ID3D10Buffer*>(texture) || e.sub != sub)
            continue;
        const RingMap m = e;
        memmove(&g_ringMaps[i], &g_ringMaps[i + 1], (g_ringMapCount - i - 1) * sizeof(RingMap));
        g_ringMapCount--;
        const uint64_t freeTo = g_ringMapCount ? g_ringMaps[0].start : g_dataWrite;
        Recorder r = Begin(kOpTexUpload, 56);
        Hold(texture);
        r.Word(reinterpret_cast<uint64_t>(texture));
        r.Word(m.start);
        r.Word(freeTo);
        r.Word(m.sub | static_cast<uint64_t>(m.rowBytes) << 32);
        r.Word(m.rowPitch | static_cast<uint64_t>(m.rows) << 32);
        r.Word(m.slices | static_cast<uint64_t>(m.depthPitch) << 32);
        r.Word(0);
        End(r);
        return;
    }
    StreamSync(kSyncTextureUnmap);
    if (const TextureVtable* o = TextureOriginals(texture))
        o->unmap(texture, sub);
}

void ExecuteTexUpload(const uint64_t* w)
{
    void* texture = reinterpret_cast<void*>(w[0]);
    const uint8_t* src = g_data + w[1] % kDataBytes;
    const UINT sub = static_cast<UINT>(w[3]), rowBytes = static_cast<UINT>(w[3] >> 32);
    const UINT rowPitch = static_cast<UINT>(w[4]), rows = static_cast<UINT>(w[4] >> 32);
    const UINT slices = static_cast<UINT>(w[5]), depthPitch = static_cast<UINT>(w[5] >> 32);
    const TextureVtable* o = TextureOriginals(texture);
    D3D10_RESOURCE_DIMENSION dim = D3D10_RESOURCE_DIMENSION_UNKNOWN;
    static_cast<ID3D10Resource*>(texture)->GetType(&dim);
    D3D10_MAPPED_TEXTURE3D m{}; // 1D: only pData; 2D: pData, RowPitch
    if (o && SUCCEEDED(o->map(texture, sub, D3D10_MAP_WRITE_DISCARD, 0, &m)) && m.pData)
    {
        auto* dst = static_cast<uint8_t*>(m.pData);
        const UINT dstRow = dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D ? rowBytes : m.RowPitch;
        const UINT dstSlice = dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? m.DepthPitch : dstRow * rows;
        for (UINT s = 0; s < slices; s++)
            for (UINT y = 0; y < rows; y++)
                memcpy(dst + s * dstSlice + y * dstRow, src + s * depthPitch + y * rowPitch, rowBytes);
        o->unmap(texture, sub);
    }
    g_dataRead.store(w[2], std::memory_order_release);
}
struct MapCaller
{
    uintptr_t at;
    uint64_t calls;
    double waitMs;
};
MapCaller g_mapCallers[16];

HRESULT STDMETHODCALLTYPE StreamTextureMap(void* texture, UINT sub, D3D10_MAP type, UINT flags, void* out)
{
    if (g_asyncTexMaps && type == D3D10_MAP_WRITE_DISCARD && out && g_ringMapCount < kMaxRingMaps && TextureMapToRing(texture, sub, out))
        return S_OK;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    StreamSync(kSyncTextureMap);
    QueryPerformanceCounter(&t1);
    for (MapCaller& c : g_mapCallers)
        if (c.at == caller || !c.at)
        {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            c.at = caller;
            c.calls++;
            c.waitMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
            break;
        }
    const TextureVtable* o = TextureOriginals(texture);
    return o ? o->map(texture, sub, type, flags, out) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE StreamQueryGetData(ID3D10Asynchronous* query, void* data, UINT size, UINT flags)
{
    NoteRecorder();
    g_queryPolls++;
    const uint64_t* pos = QueryEndSlot(query, false);
    if (!pos)
        return S_FALSE; // never ended through the stream
    if (g_readSeen < *pos && (g_readSeen = g_streamRead.load(std::memory_order_acquire)) < *pos)
    {
        g_queryBehind++;
        StreamPublish();
        return S_FALSE;
    }
    uint64_t flushSerial = 0;
    const HRESULT hr = Dx12QueryPoll(query, data, size, flags, &flushSerial);
    if (flushSerial > g_queryFlushSerial)
    {
        g_queryFlushSerial = flushSerial;
        g_queryFlushes++;
        Recorder r = Begin(kOpQueryFlush, 8);
        r.Word(flushSerial);
        End(r);
        StreamPublish();
    }
    return hr;
}

constexpr int kMaxHookedVtables = 32;
const void* g_hookedVtables[kMaxHookedVtables];
std::atomic<int> g_hookedCount{0};
SRWLOCK g_hookLock = SRWLOCK_INIT;

void HookObject(const void* object, HookKind kind)
{
    const auto* vtable = *static_cast<const void* const*>(object);
    const int known = g_hookedCount.load(std::memory_order_acquire);
    for (int i = 0; i < known; i++)
        if (g_hookedVtables[i] == vtable)
            return;
    AcquireSRWLockExclusive(&g_hookLock);
    const int count = g_hookedCount.load(std::memory_order_relaxed);
    bool seen = false;
    for (int i = 0; i < count && !seen; i++)
        seen = g_hookedVtables[i] == vtable;
    const int buffers = g_bufferVtableCount.load(std::memory_order_relaxed);
    if (!seen && count < kMaxHookedVtables && kind == kHookBuffer && buffers < kMaxBufferVtables)
    {
        // Buffers map through the data ring; the originals are registered before the vtable points at the hooks.
        auto* table = const_cast<uintptr_t*>(reinterpret_cast<const uintptr_t*>(vtable));
        g_bufferVtables[buffers] = {vtable, reinterpret_cast<StreamBufferMapFn>(table[10]), reinterpret_cast<StreamBufferUnmapFn>(table[11])};
        g_bufferVtableCount.store(buffers + 1, std::memory_order_release);
        PatchSlot(table, 10, reinterpret_cast<uintptr_t>(&StreamBufferMap));
        PatchSlot(table, 11, reinterpret_cast<uintptr_t>(&StreamBufferUnmap));
        g_hookedVtables[count] = vtable;
        g_hookedCount.store(count + 1, std::memory_order_release);
        char line[96];
        snprintf(line, sizeof(line), "stream: buffer vtable %p maps through the data ring", vtable);
        Trace(line);
    }
    else if (!seen && count < kMaxHookedVtables && kind == kHookAsync && g_asyncQueries && !g_queryVtable)
    {
        auto* table = const_cast<uintptr_t*>(reinterpret_cast<const uintptr_t*>(vtable));
        g_queryBegin = reinterpret_cast<QueryBeginEndFn>(table[7]);
        g_queryEnd = reinterpret_cast<QueryBeginEndFn>(table[8]);
        g_queryGetData = reinterpret_cast<QueryGetDataFn>(table[9]);
        g_queryVtable = vtable;
        PatchSlot(table, 7, reinterpret_cast<uintptr_t>(&StreamQueryBegin));
        PatchSlot(table, 8, reinterpret_cast<uintptr_t>(&StreamQueryEnd));
        PatchSlot(table, 9, reinterpret_cast<uintptr_t>(&StreamQueryGetData));
        g_hookedVtables[count] = vtable;
        g_hookedCount.store(count + 1, std::memory_order_release);
        char line[96];
        snprintf(line, sizeof(line), "stream: query vtable %p asynchronous (D3D12)", vtable);
        Trace(line);
    }
    else if (!seen && count < kMaxHookedVtables)
    {
        const SyncSlot* slots = kind == kHookBuffer ? kBufferSyncs : kind == kHookTexture ? kTextureSyncs : kind == kHookAsync ? kAsyncSyncs : kSwapChainSyncs;
        const size_t n = kind == kHookBuffer ? std::size(kBufferSyncs)
                         : kind == kHookTexture ? std::size(kTextureSyncs)
                         : kind == kHookAsync ? std::size(kAsyncSyncs)
                                              : std::size(kSwapChainSyncs);
        auto* table = const_cast<uintptr_t*>(reinterpret_cast<const uintptr_t*>(vtable));
        for (size_t i = 0; i < n; i++)
        {
            if (kind == kHookTexture && g_asyncQueries && (slots[i].slot == 10 || slots[i].slot == 11) && g_textureVtableCount < 4)
            {
                if (slots[i].slot == 10)
                {
                    g_textureVtables[g_textureVtableCount++] = {vtable, reinterpret_cast<TextureMapFn>(table[10]), reinterpret_cast<TextureUnmapFn>(table[11])};
                    PatchSlot(table, 10, reinterpret_cast<uintptr_t>(&StreamTextureMap));
                    PatchSlot(table, 11, reinterpret_cast<uintptr_t>(&StreamTextureUnmap));
                }
                continue;
            }
            // The first swap chain's Present goes through the stream (one original kept); others sync.
            if (kind == kHookSwapChain && slots[i].slot == 8 && !g_presentOriginal)
            {
                g_presentOriginal = reinterpret_cast<PresentFn>(table[8]);
                g_presentVtable = vtable;
                PatchSlot(table, 8, reinterpret_cast<uintptr_t>(&StreamPresent));
                continue;
            }
            if (const void* stub = EmitSyncStub(slots[i].reason, table[slots[i].slot]))
                PatchSlot(table, slots[i].slot, reinterpret_cast<uintptr_t>(stub));
        }
        g_hookedVtables[count] = vtable;
        g_hookedCount.store(count + 1, std::memory_order_release);
        const auto offset = reinterpret_cast<const uint8_t*>(vtable) - static_cast<const uint8_t*>(object);
        char line[128];
        snprintf(line, sizeof(line), "stream: hooked kind %d vtable %p (object %+lld)", kind, vtable, static_cast<long long>(offset));
        Trace(line);
    }
    ReleaseSRWLockExclusive(&g_hookLock);
}

// Creation through the stream device: forwarded, then the new object's vtable gets its sync stubs. The out pointer is
// always the last argument.
template <int Slot, HookKind Kind, typename... A>
HRESULT STDMETHODCALLTYPE C_Create(ID3D10Device1* This, A... args, void** out)
{
    Count(Slot);
    const HRESULT hr = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(ID3D10Device1*, A..., void**)>(RealSlot(This, Slot))(Real(This), args..., out);
    if (SUCCEEDED(hr) && out && *out)
        HookObject(*out, Kind);
    return hr;
}

// The swap chain is created on the factory with the device; its Present and mode changes sync.
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
CreateSwapChainFn g_createSwapChain;

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    // The D3D12 device, given directly or through the stream's wrapper.
    if (Dx12OwnsDevice(device) || (g_d3d12Device && static_cast<void*>(device) == static_cast<void*>(g_d3d12Device)))
    {
        const HRESULT hr = Dx12CreateSwapChain(device, desc, out);
        if (SUCCEEDED(hr) && g_streamDevice)
            HookObject(*out, kHookSwapChain);
        return hr;
    }
    if (desc)
    {
        char line[200];
        snprintf(line, sizeof(line), "swap chain: %ux%u fmt=%u refresh=%u/%u buffers=%u usage=0x%x effect=%u flags=0x%x windowed=%d msaa=%u",
                 desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format, desc->BufferDesc.RefreshRate.Numerator,
                 desc->BufferDesc.RefreshRate.Denominator, desc->BufferCount, desc->BufferUsage, desc->SwapEffect, desc->Flags, desc->Windowed,
                 desc->SampleDesc.Count);
        Trace(line);
    }
    const HRESULT hr = g_createSwapChain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out)
        HookObject(*out, kHookSwapChain);
    return hr;
}

// The factory implementation's vtable is shared by every factory, so patching it through any factory catches the
// renderer's CreateSwapChain.
void HookFactoryVtable(IDXGIFactory* factory)
{
    auto* table = *reinterpret_cast<uintptr_t**>(factory);
    if (table[10] == reinterpret_cast<uintptr_t>(&HookCreateSwapChain))
        return;
    g_createSwapChain = reinterpret_cast<CreateSwapChainFn>(table[10]);
    PatchSlot(table, 10, reinterpret_cast<uintptr_t>(&HookCreateSwapChain));
    Trace("factory CreateSwapChain hooked");
}

void HookFactoryOf(ID3D10Device1* device)
{
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
        HookFactoryVtable(factory);
    else
        Trace("stream: no factory, Present is not synced");
    if (factory)
        factory->Release();
    if (adapter)
        adapter->Release();
    if (dxgi)
        dxgi->Release();
}

// For the D3D12 device: the factory of the renderer's adapter, or a new one.
void HookFactoryOfAdapter(IDXGIAdapter* adapter)
{
    IDXGIFactory* factory = nullptr;
    if (adapter)
        adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    if (!factory)
    {
        using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        if (auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "CreateDXGIFactory")))
            create(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    }
    if (!factory)
    {
        Trace("d3d12: no DXGI factory to hook");
        return;
    }
    HookFactoryVtable(factory);
    factory->Release();
}

// Texture creation is noted with its size and caller: at 5120x1440 the renderer creates about 9 a frame.
__attribute__((noinline)) HRESULT STDMETHODCALLTYPE C_CreateTexture2D(ID3D10Device1* This, const D3D10_TEXTURE2D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init,
                                                                      void** out)
{
    if (desc)
    {
        const void* caller = __builtin_return_address(0);
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(caller), &module);
        char key[192];
        snprintf(key, sizeof(key), "Tex2D %ux%u mips=%u array=%u fmt=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x init=%d caller=+0x%llx", desc->Width,
                 desc->Height, desc->MipLevels, desc->ArraySize, desc->Format, desc->Usage, desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags,
                 init != nullptr, static_cast<unsigned long long>(static_cast<const uint8_t*>(caller) - reinterpret_cast<const uint8_t*>(module)));
        Note(g_created, key);
    }
    return C_Create<73, kHookTexture, const D3D10_TEXTURE2D_DESC*, const D3D10_SUBRESOURCE_DATA*>(This, desc, init, out);
}

// Shader binding census (with CRYMT_D3DCENSUS): which constant buffer, texture and sampler slots each stage's shaders
// declare, to lay out the D3D12 root signature.
struct StageUse
{
    uint64_t shaders;
    uint64_t cbSlot[16];      // shaders using constant buffer slot i
    uint32_t cbBytes[16];     // largest constant buffer seen in slot i
    uint64_t cbCount[16];     // shaders declaring n constant buffers
    uint64_t texEnd[129];     // shaders whose highest texture slot + 1 is n
    uint64_t samplerEnd[17];  // shaders whose highest sampler slot + 1 is n
};
StageUse g_stageUse[3]; // VS, GS, PS

// Resolved at run time: an import of d3d10.dll would load it before CryMT can intercept D3D10CreateDevice.
using ReflectShaderFn = HRESULT(WINAPI*)(const void*, SIZE_T, ID3D10ShaderReflection**);

void NoteShader(int stage, const void* code, SIZE_T bytes)
{
    static const auto reflect = reinterpret_cast<ReflectShaderFn>(GetProcAddress(GetModuleHandleW(L"d3d10.dll"), "D3D10ReflectShader"));
    ID3D10ShaderReflection* reflection = nullptr;
    if (!reflect || FAILED(reflect(code, bytes, &reflection)) || !reflection)
        return;
    D3D10_SHADER_DESC desc{};
    reflection->GetDesc(&desc);
    EnterCriticalSection(&g_censusLock);
    StageUse& use = g_stageUse[stage];
    use.shaders++;
    UINT cbs = 0, texEnd = 0, samplerEnd = 0;
    for (UINT i = 0; i < desc.BoundResources; i++)
    {
        D3D10_SHADER_INPUT_BIND_DESC bind{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &bind)))
            continue;
        const UINT end = bind.BindPoint + bind.BindCount;
        if (bind.Type == D3D10_SIT_CBUFFER && bind.BindPoint < 16)
        {
            cbs++;
            use.cbSlot[bind.BindPoint]++;
            D3D10_SHADER_BUFFER_DESC cb{};
            if (ID3D10ShaderReflectionConstantBuffer* buffer = reflection->GetConstantBufferByName(bind.Name); buffer && SUCCEEDED(buffer->GetDesc(&cb)))
                use.cbBytes[bind.BindPoint] = std::max(use.cbBytes[bind.BindPoint], static_cast<uint32_t>(cb.Size));
        }
        else if (bind.Type == D3D10_SIT_TEXTURE || bind.Type == D3D10_SIT_TBUFFER)
            texEnd = std::max(texEnd, end);
        else if (bind.Type == D3D10_SIT_SAMPLER)
            samplerEnd = std::max(samplerEnd, end);
    }
    use.cbCount[std::min(cbs, 15u)]++;
    use.texEnd[std::min(texEnd, 128u)]++;
    use.samplerEnd[std::min(samplerEnd, 16u)]++;
    LeaveCriticalSection(&g_censusLock);
    reflection->Release();
}

template <int Slot, int Stage>
HRESULT STDMETHODCALLTYPE C_CreateShader(ID3D10Device1* This, const void* code, SIZE_T bytes, void** out)
{
    if (g_censusOn && code)
        NoteShader(Stage, code, bytes);
    Count(Slot);
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(ID3D10Device1*, const void*, SIZE_T, void**)>(RealSlot(This, Slot))(Real(This), code, bytes, out);
}

void WriteStageUse(FILE* f)
{
    static const char* const kStages[3] = {"VS", "GS", "PS"};
    for (int s = 0; s < 3; s++)
    {
        const StageUse& use = g_stageUse[s];
        if (!use.shaders)
            continue;
        fprintf(f, "%s shaders %llu\n  cb slots (shaders, largest bytes):", kStages[s], static_cast<unsigned long long>(use.shaders));
        for (int i = 0; i < 16; i++)
            if (use.cbSlot[i])
                fprintf(f, " b%d=%llu/%u", i, static_cast<unsigned long long>(use.cbSlot[i]), use.cbBytes[i]);
        fprintf(f, "\n  cbs per shader:");
        for (int i = 0; i < 16; i++)
            if (use.cbCount[i])
                fprintf(f, " %d:%llu", i, static_cast<unsigned long long>(use.cbCount[i]));
        fprintf(f, "\n  highest texture slot+1:");
        for (int i = 0; i < 129; i++)
            if (use.texEnd[i])
                fprintf(f, " %d:%llu", i, static_cast<unsigned long long>(use.texEnd[i]));
        fprintf(f, "\n  highest sampler slot+1:");
        for (int i = 0; i < 17; i++)
            if (use.samplerEnd[i])
                fprintf(f, " %d:%llu", i, static_cast<unsigned long long>(use.samplerEnd[i]));
        fprintf(f, "\n");
    }
}

void* g_streamVtable[kWrapSlots];

// The stream device's vtable: the wrapper's forwarders, with the immediate context recorded, the order-dependent calls
// synced and creation hooking the new objects.
void BuildStreamVtable()
{
    memcpy(g_streamVtable, kWrapVtable, sizeof(g_streamVtable));
    auto set = [](int slot, auto fn) { g_streamVtable[slot] = reinterpret_cast<void*>(fn); };
    set(3, &R_SetArray<3>);
    set(4, &R_SetArray<4>);
    set(6, &R_SetArray<6>);
    set(10, &R_SetArray<10>);
    set(16, &R_SetArray<16>);
    set(19, &R_SetArray<19>);
    set(20, &R_SetArray<20>);
    set(22, &R_SetArray<22>);
    set(23, &R_SetArray<23>);
    set(5, &R_SetObject<5>);
    set(7, &R_SetObject<7>);
    set(11, &R_SetObject<11>);
    set(17, &R_SetObject<17>);
    set(29, &R_SetObject<29>);
    set(8, &R_Ints3<8>);
    set(9, &R_Ints2<9>);
    set(14, &R_DrawIndexedInstanced);
    set(15, &R_DrawInstanced);
    set(18, &R_IASetPrimitiveTopology);
    set(12, &R_IASetVertexBuffers);
    set(13, &R_IASetIndexBuffer);
    set(24, &R_OMSetRenderTargets);
    set(25, &R_OMSetBlendState);
    set(26, &R_OMSetDepthStencilState);
    set(30, &R_RSSetViewports);
    set(31, &R_RSSetScissorRects);
    set(32, &R_CopySubresourceRegion);
    set(33, &R_CopyResource);
    set(34, &R_UpdateSubresource);
    set(35, &R_ClearRenderTargetView);
    set(36, &R_ClearDepthStencilView);
    set(37, &R_GenerateMips);
    set(38, &R_ResolveSubresource);
    set(28, &R_NoArgs<28>);
    set(69, &R_NoArgs<69>);
    set(70, &R_NoArgs<70>);
    // Order-dependent or state-reading calls sync first (all rare): SetPredication, SOSetTargets, UpdateSubresource,
    // the getters.
    for (int s : {21, 27, 34})
        g_syncBefore[s] = true;
    for (int s = 39; s <= 62; s++)
        g_syncBefore[s] = true;
    set(71, &C_Create<71, kHookBuffer, const D3D10_BUFFER_DESC*, const D3D10_SUBRESOURCE_DATA*>);
    set(72, &C_Create<72, kHookTexture, const D3D10_TEXTURE1D_DESC*, const D3D10_SUBRESOURCE_DATA*>);
    set(73, &C_CreateTexture2D);
    set(79, &C_CreateShader<79, 0>);
    set(80, &C_CreateShader<80, 1>);
    set(82, &C_CreateShader<82, 2>);
    set(74, &C_Create<74, kHookTexture, const D3D10_TEXTURE3D_DESC*, const D3D10_SUBRESOURCE_DATA*>);
    set(87, &C_Create<87, kHookAsync, const D3D10_QUERY_DESC*>);
    set(88, &C_Create<88, kHookAsync, const D3D10_QUERY_DESC*>);
}

void StartStream(ID3D10Device1* device)
{
    {
        wchar_t v[8] = {};
        g_asyncPresent = !(GetEnvironmentVariableW(L"CRYMT_ASYNCPRESENT", v, 8) && wcscmp(v, L"0") == 0);
    }
    g_stream = static_cast<uint8_t*>(VirtualAlloc(nullptr, kStreamBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_data = static_cast<uint8_t*>(VirtualAlloc(nullptr, kDataBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_syncStubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSyncStubs * kSyncStubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    g_streamWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stream || !g_data || !g_syncStubs || !g_streamWake)
    {
        Trace("stream: allocation failed, stream off");
        return;
    }
    g_held.reserve(kMaxHeld);
    BuildStreamVtable();
    HookFactoryOf(device);
    g_streamDevice = device;
    HANDLE thread = CreateThread(nullptr, 0, &StreamMain, nullptr, 0, nullptr);
    if (!thread)
    {
        g_streamDevice = nullptr;
        Trace("stream: thread failed, stream off");
        return;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto setDescription = reinterpret_cast<SetDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
        setDescription(thread, L"CryMT D3D stream");
    CloseHandle(thread);
    Trace("stream: running");
    SetupLog("d3d command stream running");
}

ID3D10Device* WrapDeviceOf(ID3D10Device* real, bool stream)
{
    ID3D10Device1* real1 = nullptr;
    if (FAILED(real->QueryInterface(__uuidof(ID3D10Device1), reinterpret_cast<void**>(&real1))) || !real1)
        return real;
    real->Release(); // the wrapper holds the reference D3D10CreateDevice returned, as ID3D10Device1
    auto* wrap = new WrapDevice{kWrapVtable, real1};
    if (stream)
    {
        StartStream(real1);
        if (g_streamDevice)
            wrap->vtbl = g_streamVtable;
    }
    return reinterpret_cast<ID3D10Device*>(wrap);
}

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);
CreateDeviceFn g_createDevice;

// The engine modules load at their preferred bases and CryMT patches them at fixed addresses. The D3D12 device and its
// first resources are created before Cry3DEngine and others load, and the driver's address space reservations took
// Cry3DEngine's range (it was relocated and the patches faulted). So the ranges of the game's modules not loaded yet
// are reserved before the device is created, and each is released when CrySystem loads that module.
std::mutex g_reservedLock;
std::vector<std::pair<void*, std::wstring>> g_reserved;

void ReleaseReservedRange(const wchar_t* path)
{
    if (!path)
        return;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* fwd = wcsrchr(path, L'/');
    const wchar_t* file = std::max(slash, fwd) ? std::max(slash, fwd) + 1 : path;
    std::lock_guard<std::mutex> guard(g_reservedLock);
    for (auto it = g_reserved.begin(); it != g_reserved.end(); ++it)
    {
        const size_t len = it->second.size();
        // The name may come without the extension.
        if (_wcsnicmp(file, it->second.c_str(), len - 4) == 0 && (file[len - 4] == 0 || _wcsicmp(file + len - 4, L".dll") == 0))
        {
            VirtualFree(it->first, 0, MEM_RELEASE);
            g_reserved.erase(it);
            return;
        }
    }
}

void ReleaseReservedRangeA(const char* path)
{
    if (!path)
        return;
    wchar_t wide[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH))
        ReleaseReservedRange(wide);
}

HMODULE WINAPI ReserveLoadLibraryA(LPCSTR name)
{
    ReleaseReservedRangeA(name);
    return LoadLibraryA(name);
}

HMODULE WINAPI ReserveLoadLibraryW(LPCWSTR name)
{
    ReleaseReservedRange(name);
    return LoadLibraryW(name);
}

HMODULE WINAPI ReserveLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    ReleaseReservedRangeA(name);
    return LoadLibraryExA(name, file, flags);
}

HMODULE WINAPI ReserveLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    ReleaseReservedRange(name);
    return LoadLibraryExW(name, file, flags);
}

// The preferred ranges of the Bin64 DLLs not loaded yet (or only those named in `only`), held until CrySystem loads them.
std::vector<std::pair<void*, std::wstring>> ReserveModuleRanges(const wchar_t* const* only = nullptr, size_t count = 0)
{
    std::vector<std::pair<void*, std::wstring>> reserved;
    wchar_t dir[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* slash = n ? wcsrchr(dir, L'\\') : nullptr;
    if (!slash)
        return reserved;
    slash[1] = 0;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((std::wstring(dir) + L"*.dll").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return reserved;
    do
    {
        if (GetModuleHandleW(found.cFileName))
            continue;
        bool wanted = !only;
        for (size_t i = 0; i < count && !wanted; i++)
            wanted = _wcsicmp(found.cFileName, only[i]) == 0;
        if (!wanted)
            continue;
        const std::wstring path = std::wstring(dir) + found.cFileName;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            continue;
        uint8_t header[4096];
        DWORD read = 0;
        const bool ok = ReadFile(file, header, sizeof(header), &read, nullptr) && read >= sizeof(IMAGE_DOS_HEADER);
        CloseHandle(file);
        if (!ok)
            continue;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(header);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > read)
            continue;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(header + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            continue;
        void* base = reinterpret_cast<void*>(nt->OptionalHeader.ImageBase);
        if (void* p = VirtualAlloc(base, nt->OptionalHeader.SizeOfImage, MEM_RESERVE, PAGE_NOACCESS))
            reserved.push_back({p, found.cFileName});
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return reserved;
}

HRESULT WINAPI HookD3D10CreateDevice(IDXGIAdapter* adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, UINT sdk, ID3D10Device** out)
{
    ::CryMTForceNoMsaa(); // before the renderer makes its targets
    ::CryMTInstallPakLog(); // the renderer's gEnv is set by now, its shaders not loaded yet
    char line[128];
    snprintf(line, sizeof(line), "D3D10CreateDevice called: type=%d flags=0x%x sdk=%u original=%p", type, flags, sdk,
             reinterpret_cast<void*>(g_createDevice));
    Trace(line);
    // The renderer's final device is the one it asks to be single-threaded.
    if (g_d3d12On && !g_d3d12Device && (flags & D3D10_CREATE_DEVICE_SINGLETHREADED) && out)
    {
        {
            std::lock_guard<std::mutex> guard(g_reservedLock);
            auto more = ReserveModuleRanges();
            g_reserved.insert(g_reserved.end(), more.begin(), more.end());
        }
        static const char* const kLoads[] = {"LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW"};
        static const void* const kHooks[] = {reinterpret_cast<const void*>(&ReserveLoadLibraryA), reinterpret_cast<const void*>(&ReserveLoadLibraryW),
                                             reinterpret_cast<const void*>(&ReserveLoadLibraryExA), reinterpret_cast<const void*>(&ReserveLoadLibraryExW)};
        const int hooked = CryMTPatchImports(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"CrySystem.dll")), "KERNEL32.dll", kLoads, kHooks, 4);
        ID3D10Device* device = Dx12CreateDevice(adapter, flags);
        // RenderDoc (CRYMT_RENDERDOC), loaded by the back end before its device, rewrote CrySystem's LoadLibrary imports to
        // its own hooks, so the reserved ranges were not released and Cry3DEngine was relocated. Ours go back on top; they
        // call LoadLibrary through CryMT's imports, which RenderDoc hooks too, so it still sees every load.
        if (GetModuleHandleW(L"renderdoc.dll"))
        {
            const int again = CryMTPatchImports(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"CrySystem.dll")), "KERNEL32.dll", kLoads, kHooks, 4);
            snprintf(line, sizeof(line), "d3d12: RenderDoc loaded, CrySystem's LoadLibrary imports hooked again (%d)", again);
            Trace(line);
        }
        snprintf(line, sizeof(line), "d3d12: %zu module ranges reserved until their modules load (%d CrySystem imports)", g_reserved.size(), hooked);
        Trace(line);
        if (device)
        {
            // With the stream as well, the renderer records and the stream thread translates to D3D12.
            if (g_streamOn)
            {
                device = WrapDeviceOf(device, true);
                wchar_t v[8];
                g_directMaps = !(GetEnvironmentVariableW(L"CRYMT_DIRECTMAPS", v, 8) && wcscmp(v, L"0") == 0);
                g_asyncQueries = !(GetEnvironmentVariableW(L"CRYMT_ASYNCQUERIES", v, 8) && wcscmp(v, L"0") == 0);
                g_asyncTexMaps = !(GetEnvironmentVariableW(L"CRYMT_ASYNCTEXMAP", v, 8) && wcscmp(v, L"0") == 0);
                Trace(g_directMaps ? "stream: dynamic buffers map directly (D3D12 upload memory)" : "stream: dynamic buffers map through the data ring");
            }
            g_d3d12Device = device;
            *out = device;
            HookFactoryOfAdapter(adapter);
            Trace("D3D12 device created for the renderer");
            SetupLog("d3d12 back end: renderer device created on D3D12");
            return S_OK;
        }
        Trace("D3D12 device failed, using D3D10 (see d3d12.txt)");
        SetupLog("d3d12 back end failed, D3D10 used");
    }
    const bool stream = g_streamOn && !g_streamDevice && (flags & D3D10_CREATE_DEVICE_SINGLETHREADED);
    if (stream)
        flags &= ~D3D10_CREATE_DEVICE_SINGLETHREADED;
    const HRESULT hr = g_createDevice(adapter, type, software, flags, sdk, out);
    snprintf(line, sizeof(line), "D3D10CreateDevice: hr=%08lx device=%p vtable=%p", static_cast<unsigned long>(hr), out ? static_cast<void*>(*out) : nullptr,
             out && *out ? *reinterpret_cast<void**>(*out) : nullptr);
    Trace(line);
    SetupLog(line);
    if (SUCCEEDED(hr) && out && *out && g_censusOn && !g_wrapOn)
    {
        InstallCensus(*out);
        Trace("census installed");
    }
    if (SUCCEEDED(hr) && out && *out && g_wrapOn)
    {
        *out = WrapDeviceOf(*out, stream);
        Trace(stream ? "device wrapped, with the command stream" : "device wrapped");
    }
    return hr;
}
} // namespace

// Called for every module load (CryMT's LdrRegisterDllNotification callback).
void D3DLayerOnDll(const wchar_t* name, size_t chars, uintptr_t base)
{
    if (chars != 9 || _wcsnicmp(name, L"d3d10.dll", 9) != 0)
        return;
    // The renderer is loading (d3d10.dll, before its own imports such as dxgi.dll): the engine modules CryMT patches that
    // CrySystem loads after it get their address ranges held until CrySystem's LoadLibrary asks for them. Otherwise an
    // overlay that comes with the renderer (ReShade as Bin64\dxgi.dll, its add-ons) can take that memory first, the module
    // lands elsewhere and CryMT turns off ("CryAnimation.dll is not the ... 6156 module").
    {
        static const wchar_t* const kLater[] = {L"Cry3DEngine.dll", L"CryAnimation.dll"};
        std::lock_guard<std::mutex> guard(g_reservedLock);
        auto held = ReserveModuleRanges(kLater, 2);
        g_reserved.insert(g_reserved.end(), held.begin(), held.end());
        static const char* const kLoads[] = {"LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW"};
        static const void* const kHooks[] = {reinterpret_cast<const void*>(&ReserveLoadLibraryA), reinterpret_cast<const void*>(&ReserveLoadLibraryW),
                                             reinterpret_cast<const void*>(&ReserveLoadLibraryExA), reinterpret_cast<const void*>(&ReserveLoadLibraryExW)};
        const int hooked = held.empty() ? 0 : CryMTPatchImports(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"CrySystem.dll")), "KERNEL32.dll", kLoads, kHooks, 4);
        char line[96];
        snprintf(line, sizeof(line), "engine ranges held before the renderer's imports: %d (CrySystem imports hooked %d)", static_cast<int>(held.size()), hooked);
        SetupLog(line);
    }
    g_censusOn = GetEnvironmentVariableW(L"CRYMT_D3DCENSUS", nullptr, 0) != 0;
    g_d3d12On = GetEnvironmentVariableW(L"CRYMT_D3D12", nullptr, 0) != 0;
    g_streamOn = GetEnvironmentVariableW(L"CRYMT_D3DSTREAM", nullptr, 0) != 0;
    g_wrapOn = g_streamOn || (!g_d3d12On && GetEnvironmentVariableW(L"CRYMT_D3DWRAP", nullptr, 0) != 0);
    if (!g_censusOn && !g_wrapOn && !g_d3d12On)
        return;
    InitializeCriticalSection(&g_censusLock);
    g_createDevice = reinterpret_cast<CreateDeviceFn>(PatchModuleExport(base, "D3D10CreateDevice", reinterpret_cast<const void*>(&HookD3D10CreateDevice)));
    char line[96];
    snprintf(line, sizeof(line), "d3d10 loaded at %p, D3D10CreateDevice original %p", reinterpret_cast<void*>(base), reinterpret_cast<void*>(g_createDevice));
    Trace(line);
    SetupLog(g_createDevice ? "d3d10 D3D10CreateDevice redirected" : "d3d10 D3D10CreateDevice not found");
}

// The scene is complete (CryMT, render thread, before HDR post-processing): in stream order on the D3D12 back end.
void D3DLayerSceneDone(const DlssSceneInfo* info)
{
    if (!g_d3d12Device)
        return;
    if (g_streamDevice)
    {
        Recorder r = Begin(kOpScene, sizeof(DlssSceneInfo));
        r.Bytes(info, sizeof(DlssSceneInfo));
        End(r);
    }
    else
        Dx12SceneDone(info);
}

// The main camera was set (CryMT, render thread): in stream order, so the back end knows it before the frame's draws.
void D3DLayerCamera(const DlssSceneInfo* info)
{
    if (!g_d3d12Device)
        return;
    if (g_streamDevice)
    {
        Recorder r = Begin(kOpCamera, sizeof(DlssSceneInfo));
        r.Bytes(info, sizeof(DlssSceneInfo));
        End(r);
    }
    else
        Dx12Camera(info);
}

// A pass name for frame debuggers (RenderDoc, PIX), in stream order; null ends the innermost one.
void D3DLayerPassName(const char* text)
{
    if (!g_d3d12Device)
        return;
    const char* t = text ? text : "";
    if (g_streamDevice)
    {
        const uint32_t n = static_cast<uint32_t>(strlen(t)) + 1;
        Recorder r = Begin(kOpPassName, n);
        r.Bytes(t, n);
        End(r);
    }
    else
        Dx12PassName(t);
}

// DLSS runs and the back buffer size is known (CryMT's camera jitter).
bool D3DLayerDlssScreen(uint32_t* width, uint32_t* height)
{
    return g_d3d12Device && Dx12DlssActive() && Dx12ScreenSize(width, height);
}

// The D3D12 back end runs and the back buffer size is known (the scene hand-over for GI, without DLSS).
bool D3DLayerScreen(uint32_t* width, uint32_t* height)
{
    return g_d3d12Device && Dx12ScreenSize(width, height);
}

// Draw packets need the D3D12 back end; in stream order when the command stream runs.
bool D3DLayerPacketsAvailable()
{
    return g_d3d12Device != nullptr;
}

// objects: the buffers the packet names, held until the stream has executed it.
void D3DLayerDrawPacket(const uint8_t* packet, uint32_t bytes, void* const* objects, int count)
{
    if (g_streamDevice)
    {
        for (int i = 0; i < count; i++)
            Hold(objects[i]);
        Recorder r = Begin(kOpDrawPacket, bytes);
        r.Bytes(packet, bytes);
        End(r);
    }
    else
        Dx12DrawPacket(packet);
}

// A group command (DrawPacket.h): expanded by the stream thread, or at once without the stream.
void D3DLayerDrawGroup(const uint8_t* payload, uint32_t bytes)
{
    if (g_streamDevice)
    {
        Recorder r = Begin(kOpDrawGroup, bytes);
        r.Bytes(payload, bytes);
        End(r);
    }
    else
        ZPassExpandGroup(payload);
}

// Recorded commands go to sink (null: stop); only while the command stream records (CRYMT_D3DSTREAM).
bool D3DLayerTrace(std::vector<uint32_t>* sink)
{
    g_trace = sink;
    return g_streamDevice != nullptr;
}

const char* D3DLayerOpName(uint32_t op)
{
    return op < 101 ? kWrapNames[op] : op == kOpUpload ? "Upload" : op == kOpScene ? "Scene" : "?";
}

// Per top-level frame; device: the interface the renderer keeps (renderer +0x2C270), which may differ from the one
// D3D10CreateDevice returned (another vtable).
void D3DLayerFrame(void* device)
{
    if (g_censusOn && !g_wrapOn && g_censusFrames == 0 && device)
    {
        char line[160];
        snprintf(line, sizeof(line), "frame device %p vtable %p; patched:", device, *static_cast<void**>(device));
        Trace(line);
        for (int i = 0; i < g_patchedCount; i++)
        {
            snprintf(line, sizeof(line), "  vtable %p draw %p", static_cast<void*>(g_patched[i].vtable), reinterpret_cast<void*>(g_patched[i].vtable[8]));
            Trace(line);
        }
        InstallCensus(static_cast<ID3D10Device*>(device));
    }
    g_censusFrames++;
}

void D3DLayerWriteStats()
{
    if (!g_censusOn && !g_wrapOn && !g_d3d12On)
        return;
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("d3d10_census.txt").c_str(), "w") != 0 || !f)
        return;
    const double frames = g_censusFrames ? static_cast<double>(g_censusFrames) : 1.0;
    if (g_d3d12Device)
        Dx12WriteStats(f, frames);
    if (g_wrapOn)
    {
        fprintf(f, "wrapped ID3D10Device1 calls (total, per frame):\n");
        for (int s = 0; s < kWrapSlots; s++)
            if (g_wrapCalls[s])
                fprintf(f, "  %3d %-40s %12llu %10.1f\n", s, kWrapNames[s], static_cast<unsigned long long>(g_wrapCalls[s]), g_wrapCalls[s] / frames);
    }
    if (g_streamDevice)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        const double ms = 1000.0 / static_cast<double>(freq.QuadPart);
        fprintf(f, "command stream: %.1f commands/frame, stream thread busy %.3f ms/frame, recorder thread changes %.2f/frame\n",
                g_streamCommands / frames, g_streamBusyTicks * ms / frames, g_recorderSwitches / frames);
        fprintf(f, "data ring: %.1f uploads/frame, %.1f KB/frame; publishes %.1f/frame; holds %.1f/frame, objects held and released %.1f/frame\n",
                g_ringUploads / frames, g_ringBytes / 1024.0 / frames, g_streamPublishes / frames, g_holds / frames, g_heldTotal / frames);
        uint64_t directDiscards = 0, directBytes = 0;
        if (g_directMaps)
            Dx12DirectMapStats(&directDiscards, &directBytes);
        fprintf(f, "direct maps: DISCARD %.1f/frame (%.1f KB/frame), NO_OVERWRITE %.1f/frame\n", directDiscards / frames,
                directBytes / 1024.0 / frames, g_directNoOverwrite / frames);
        fprintf(f, "asynchronous queries: %s; GetData %.1f/frame, before the stream reached the End %.1f/frame, flushes asked %.2f/frame\n",
                g_queryVtable ? "on" : "off", g_queryPolls / frames, g_queryBehind / frames, g_queryFlushes / frames);
        fprintf(f, "asynchronous texture maps (DISCARD through the data ring): %s, %.2f/frame\n", g_asyncTexMaps ? "on" : "off", g_texMapsAsync / frames);
        for (const MapCaller& c : g_mapCallers)
            if (c.at)
            {
                HMODULE m = nullptr;
                wchar_t mod[MAX_PATH] = L"?";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(c.at), &m))
                    GetModuleFileNameW(m, mod, MAX_PATH);
                const wchar_t* slash = wcsrchr(mod, L'\\');
                fprintf(f, "texture map caller %ls+0x%llx: %.2f/frame, wait %.3f ms/frame\n", slash ? slash + 1 : mod,
                        static_cast<unsigned long long>(c.at - reinterpret_cast<uintptr_t>(m)), c.calls / frames, c.waitMs / frames);
            }
        fprintf(f, "UpdateSubresource: through the data ring %.2f/frame, synced %.2f/frame; presents through the stream %.2f/frame\n",
                g_updatesAsync / frames, g_updatesSynced / frames, g_presentsAsync / frames);
        fprintf(f, "highest slot set (start + count):");
        for (int s = 0; s < kWrapSlots; s++)
            if (g_setEnd[s])
                fprintf(f, " %s=%u", kWrapNames[s], g_setEnd[s]);
        fprintf(f, "\nsyncs (per frame: calls, waits, wait ms):\n");
        for (int r = 0; r < kSyncReasons; r++)
            if (g_syncs[r])
                fprintf(f, "  %-40s %10.1f %10.1f %8.3f\n", r < 101 ? kWrapNames[r] : kSyncReasonNames[r - 101], g_syncs[r] / frames, g_syncWaits[r] / frames,
                        g_syncWaitTicks[r] * ms / frames);
    }
    fprintf(f, "frames=%llu\nID3D10Device calls (total, per frame):\n", static_cast<unsigned long long>(g_censusFrames));
    for (int s = 0; s < kDeviceSlots; s++)
        if (g_deviceCalls[s])
            fprintf(f, "  %3d %-40s %12lld %10.1f\n", s, kDeviceNames[s], static_cast<long long>(g_deviceCalls[s]), g_deviceCalls[s] / frames);
    EnterCriticalSection(&g_censusLock);
    WriteStageUse(f);
    fprintf(f, "created:\n");
    for (const auto& [k, v] : g_created)
        fprintf(f, "  %8llu  %s\n", static_cast<unsigned long long>(v), k.c_str());
    fprintf(f, "maps (total, per frame):\n");
    for (const auto& [k, v] : g_maps)
        fprintf(f, "  %10llu %8.1f  %s\n", static_cast<unsigned long long>(v), v / frames, k.c_str());
    LeaveCriticalSection(&g_censusLock);
    fclose(f);
}
