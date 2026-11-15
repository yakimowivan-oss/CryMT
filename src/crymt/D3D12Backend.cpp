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
// ---------------------------------------------------------------------------------------------------------------------
// Logging: logs\mt\d3d12.txt.
// ---------------------------------------------------------------------------------------------------------------------
std::mutex g_logLock;
FILE* g_log;
int g_logLines;

void Log(const char* format, ...)
{
    std::lock_guard<std::mutex> guard(g_logLock);
    if (!g_log && fopen_s(&g_log, CryMTLog("d3d12.txt").c_str(), "w") != 0)
        g_log = nullptr;
    if (!g_log || g_logLines >= 20000)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
    g_logLines++;
}

// Logs a message at most a few times per call site key.
void LogLimited(const char* key, const char* format, ...)
{
    static std::mutex lock;
    static std::unordered_map<std::string, int> counts;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (++counts[key] > 5)
            return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Log("%s", text);
}

void LogGuid(const char* what, REFIID riid)
{
    char key[96];
    snprintf(key, sizeof(key), "%s {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", what, riid.Data1, riid.Data2, riid.Data3, riid.Data4[0],
             riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
    LogLimited(key, "QueryInterface not supported: %s", key);
}

// ---------------------------------------------------------------------------------------------------------------------
// Core: device, queue, fence, submissions.
// ---------------------------------------------------------------------------------------------------------------------
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using GetDebugInterfaceFn = HRESULT(WINAPI*)(REFIID, void**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using ReflectShaderFn = HRESULT(WINAPI*)(const void*, SIZE_T, ID3D10ShaderReflection**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

constexpr D3D12_RESOURCE_STATES kSrvState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct Core
{
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t signaled = 0;  // highest value signaled on the queue
    uint64_t completed = 0; // last value read back
    ID3D12RootSignature* root = nullptr;
    ID3D12InfoQueue* info = nullptr;
    IDXGIFactory4* factory = nullptr;
    UINT srvInc = 0, rtvInc = 0, dsvInc = 0, samplerInc = 0;
    bool tearing = false;
    std::recursive_mutex lock; // allocators, uploads and submissions shared with creation on other threads
    ReflectShaderFn reflect = nullptr;
};
Core g;

uint64_t PollFence()
{
    g.completed = g.fence->GetCompletedValue();
    return g.completed;
}

void WaitFence(uint64_t value)
{
    if (PollFence() >= value)
        return;
    g.fence->SetEventOnCompletion(value, nullptr); // blocks until reached
    PollFence();
}

uint64_t SignalQueue()
{
    g.queue->Signal(g.fence, ++g.signaled);
    return g.signaled;
}

// Main lists are numbered; g_mainSerial is the one being recorded.
uint64_t g_mainSerial = 1;
std::atomic<uint64_t> g_doneSerial{0}; // every main list up to this one has completed
std::atomic<bool> g_wantPassMatrices{false}; // GI voxelizes shadow map draws: the stream sends the renderer's matrices
struct InFlight
{
    uint64_t serial, fence;
};
std::deque<InFlight> g_inFlight;

void Retire() // under g.lock
{
    PollFence();
    while (!g_inFlight.empty() && g_inFlight.front().fence <= g.completed)
    {
        g_doneSerial.store(g_inFlight.front().serial, std::memory_order_release);
        g_inFlight.pop_front();
    }
}

inline bool SerialDone(uint64_t serial)
{
    return serial <= g_doneSerial.load(std::memory_order_acquire);
}

// Waits for a submitted main list.
void WaitSerial(uint64_t serial)
{
    uint64_t fence = 0;
    {
        std::lock_guard<std::recursive_mutex> guard(g.lock);
        Retire();
        for (const InFlight& f : g_inFlight)
            if (f.serial >= serial)
            {
                fence = f.fence;
                break;
            }
    }
    if (fence)
        WaitFence(fence);
    std::lock_guard<std::recursive_mutex> guard(g.lock);
    Retire();
}

// Work freed once the main list being recorded now has completed.
struct Deferred
{
    uint64_t serial;
    IUnknown* object;
    std::function<void()> call;
};
std::deque<Deferred> g_deferred;

void DeferRelease(IUnknown* object)
{
    if (!object)
        return;
    std::lock_guard<std::recursive_mutex> guard(g.lock);
    g_deferred.push_back({g_mainSerial, object, nullptr});
}

void DeferCall(std::function<void()> call)
{
    std::lock_guard<std::recursive_mutex> guard(g.lock);
    g_deferred.push_back({g_mainSerial, nullptr, std::move(call)});
}

void RunDeferred() // under g.lock
{
    while (!g_deferred.empty() && SerialDone(g_deferred.front().serial))
    {
        Deferred d = std::move(g_deferred.front());
        g_deferred.pop_front();
        if (d.object)
            d.object->Release();
        if (d.call)
            d.call();
    }
}

struct AllocatorPool
{
    std::deque<std::pair<ID3D12CommandAllocator*, uint64_t>> used; // fence order

    ID3D12CommandAllocator* Get()
    {
        PollFence();
        if (!used.empty() && used.front().second <= g.completed)
        {
            ID3D12CommandAllocator* a = used.front().first;
            used.pop_front();
            a->Reset();
            return a;
        }
        ID3D12CommandAllocator* a = nullptr;
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&a));
        return a;
    }

    void Put(ID3D12CommandAllocator* a, uint64_t fence)
    {
        used.push_back({a, fence});
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Memory.
// ---------------------------------------------------------------------------------------------------------------------
ID3D12Resource* CreateBufferResource(D3D12_HEAP_TYPE heap, uint64_t bytes, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    const HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&res));
    if (FAILED(hr))
        Log("CreateCommittedResource buffer %llu bytes heap %d failed %08lx", static_cast<unsigned long long>(bytes), heap, static_cast<unsigned long>(hr));
    return res;
}

// Staging resources: CPU-cached memory the GPU can copy both into and out of (a readback heap can only be written),
// starting in COMMON like any buffer.
ID3D12Resource* CreateStagingBuffer(uint64_t bytes)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (bytes + 255) & ~255ull;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    const HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&res));
    if (FAILED(hr))
        Log("staging buffer %llu bytes failed %08lx", static_cast<unsigned long long>(bytes), static_cast<unsigned long>(hr));
    return res;
}

uint8_t* MapWhole(ID3D12Resource* res)
{
    void* p = nullptr;
    const D3D12_RANGE none{0, 0};
    res->Map(0, &none, &p);
    return static_cast<uint8_t*>(p);
}

struct UploadAlloc
{
    ID3D12Resource* res = nullptr;
    uint64_t offset = 0;
    uint8_t* cpu = nullptr;
};

// Upload memory handed out in submission order and reclaimed by fence.
struct UploadRing
{
    ID3D12Resource* res = nullptr;
    uint8_t* cpu = nullptr;
    uint64_t size = 0;
    uint64_t head = 0, tail = 0; // monotonic
    std::deque<std::pair<uint64_t, uint64_t>> marks; // (fence, head when submitted)

    void Init(uint64_t bytes)
    {
        size = bytes;
        res = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        cpu = res ? MapWhole(res) : nullptr;
    }

    void Reclaim()
    {
        while (!marks.empty() && marks.front().first <= g.completed)
        {
            tail = marks.front().second;
            marks.pop_front();
        }
    }

    void Mark(uint64_t fence)
    {
        if (marks.empty() || marks.back().second != head)
            marks.push_back({fence, head});
    }

    bool TryAlloc(uint64_t bytes, uint64_t align, UploadAlloc& out)
    {
        uint64_t pos = (head + align - 1) / align * align;
        if (pos % size + bytes > size)
            pos = (pos / size + 1) * size;
        if (pos + bytes - tail > size)
            return false;
        head = pos + bytes;
        out = {res, pos % size, cpu + pos % size};
        return true;
    }

    uint64_t OldestFence() const
    {
        return marks.empty() ? 0 : marks.front().first;
    }
};

// Dynamic buffer memory: upload-heap pages cut into power-of-two slices (256 B to 4 MB), larger ones dedicated.
struct Slice
{
    ID3D12Resource* res = nullptr;
    uint64_t offset = 0;
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    int cls = -1; // -1: dedicated resource
};

int SizeClass(uint64_t bytes, int classes)
{
    int c = 0;
    while ((256ull << c) < bytes)
        c++;
    return c < classes ? c : -1;
}

struct SliceHeap
{
    static constexpr int kClasses = 15;
    static constexpr uint64_t kPage = 4u << 20;
    std::vector<Slice> free[kClasses];
    ID3D12Resource* page = nullptr;
    uint8_t* pageCpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS pageGpu = 0;
    uint64_t pageUsed = kPage;
    uint64_t pages = 0, dedicated = 0;
    std::mutex m;

    Slice Alloc(uint64_t bytes)
    {
        const int c = SizeClass(bytes, kClasses);
        Slice s;
        if (c < 0)
        {
            s.res = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, (bytes + 0xFFFF) & ~0xFFFFull, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (s.res)
            {
                s.cpu = MapWhole(s.res);
                s.gpu = s.res->GetGPUVirtualAddress();
            }
            dedicated++;
            return s;
        }
        std::lock_guard<std::mutex> guard(m);
        if (!free[c].empty())
        {
            s = free[c].back();
            free[c].pop_back();
            return s;
        }
        const uint64_t bytesC = 256ull << c;
        if (pageUsed + bytesC > kPage)
        {
            page = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, kPage, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!page)
                return s;
            pageCpu = MapWhole(page);
            pageGpu = page->GetGPUVirtualAddress();
            pageUsed = 0;
            pages++;
        }
        s = {page, pageUsed, pageCpu + pageUsed, pageGpu + pageUsed, c};
        pageUsed += bytesC;
        return s;
    }

    void Free(const Slice& s) // the GPU is done with it
    {
        if (!s.res)
            return;
        if (s.cls < 0)
        {
            s.res->Release();
            return;
        }
        std::lock_guard<std::mutex> guard(m);
        free[s.cls].push_back(s);
    }

    uint64_t FreeBytes()
    {
        std::lock_guard<std::mutex> guard(m);
        uint64_t n = 0;
        for (int c = 0; c < kClasses; c++)
            n += free[c].size() * (256ull << c);
        return n;
    }
};
SliceHeap g_slices;
// How many more main lists a retired dynamic-buffer slice is kept after the one it was renamed in (CRYMT_SLICEMARGIN,
// default 2). The GI's voxel geometry replays the z-prepass draws with their buffers' GPU addresses later in the frame,
// often in a later list (a submission every 2048 draws); freed when only their own list was done, the slices were
// written again (with direct maps by the producer, a frame ahead) before the replay ran: objects stamped into the grid
// where the next frame's constants put them, voxels "boiling" on a still camera (the user, 2026-11-11).
uint64_t SliceMargin()
{
    static const uint64_t margin = [] {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"CRYMT_SLICEMARGIN", v, 8) ? static_cast<uint64_t>(_wtoi(v)) : uint64_t{2};
    }();
    return margin;
}
// Map(DISCARD) renames by kind (constant buffers, vertex/index buffers, others): how much the GPU reads from upload memory.
uint64_t g_renames[3], g_renameBytes[3];

// Default buffers: default-heap pages cut into power-of-two blocks (256 B to 16 MB), larger ones dedicated. Buffers
// decay to COMMON at the end of every ExecuteCommandLists and are promoted implicitly on first use in a list; a page
// tracks its use within the main list being recorded so a copy into it gets the right barriers.
enum PageState
{
    kPageCommon,   // not used in this list yet
    kPageRead,     // promoted implicitly to read states
    kPageAllRead,  // explicitly in GENERIC_READ
    kPageCopyDest, // copy destination
};

struct BufferPage
{
    ID3D12Resource* res = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint64_t listSerial = 0;
    PageState listState = kPageCommon;
    bool dedicated = false;
};

struct Block
{
    BufferPage* page = nullptr;
    uint64_t offset = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    int cls = -1;
};

struct BlockHeap
{
    static constexpr int kClasses = 17;
    static constexpr uint64_t kPage = 32u << 20;
    std::vector<Block> free[kClasses];
    BufferPage* page = nullptr;
    uint64_t pageUsed = kPage;
    uint64_t pages = 0, dedicated = 0;
    std::mutex m;

    Block Alloc(uint64_t bytes)
    {
        const int c = SizeClass(bytes, kClasses);
        Block b;
        if (c < 0)
        {
            auto* p = new BufferPage;
            p->res = CreateBufferResource(D3D12_HEAP_TYPE_DEFAULT, (bytes + 0xFFFF) & ~0xFFFFull, D3D12_RESOURCE_STATE_COMMON);
            if (!p->res)
            {
                delete p;
                return b;
            }
            p->gpu = p->res->GetGPUVirtualAddress();
            p->dedicated = true;
            dedicated++;
            return {p, 0, p->gpu, -1};
        }
        std::lock_guard<std::mutex> guard(m);
        if (!free[c].empty())
        {
            b = free[c].back();
            free[c].pop_back();
            return b;
        }
        const uint64_t bytesC = 256ull << c;
        if (pageUsed + bytesC > kPage)
        {
            auto* p = new BufferPage;
            p->res = CreateBufferResource(D3D12_HEAP_TYPE_DEFAULT, kPage, D3D12_RESOURCE_STATE_COMMON);
            if (!p->res)
            {
                delete p;
                return b;
            }
            p->gpu = p->res->GetGPUVirtualAddress();
            page = p;
            pageUsed = 0;
            pages++;
        }
        b = {page, pageUsed, page->gpu + pageUsed, c};
        pageUsed += bytesC;
        return b;
    }

    void Free(const Block& b) // the GPU is done with it
    {
        if (!b.page)
            return;
        if (b.cls < 0)
        {
            b.page->res->Release();
            delete b.page;
            return;
        }
        std::lock_guard<std::mutex> guard(m);
        free[b.cls].push_back(b);
    }

    uint64_t FreeBytes()
    {
        std::lock_guard<std::mutex> guard(m);
        uint64_t n = 0;
        for (int c = 0; c < kClasses; c++)
            n += free[c].size() * (256ull << c);
        return n;
    }
};
BlockHeap g_blocks;

// ---------------------------------------------------------------------------------------------------------------------
// Descriptors.
// ---------------------------------------------------------------------------------------------------------------------
struct CpuDescriptors
{
    D3D12_DESCRIPTOR_HEAP_TYPE type{};
    UINT inc = 0;
    std::vector<SIZE_T> freeList;
    SIZE_T next = 0, end = 0;
    std::mutex m;

    D3D12_CPU_DESCRIPTOR_HANDLE Alloc()
    {
        std::lock_guard<std::mutex> guard(m);
        if (!freeList.empty())
        {
            const SIZE_T h = freeList.back();
            freeList.pop_back();
            return {h};
        }
        if (next == end)
        {
            D3D12_DESCRIPTOR_HEAP_DESC d{type, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            ID3D12DescriptorHeap* heap = nullptr;
            g.dev->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
            next = heap->GetCPUDescriptorHandleForHeapStart().ptr;
            end = next + 4096 * static_cast<SIZE_T>(inc);
        }
        const SIZE_T h = next;
        next += inc;
        return {h};
    }

    void Free(D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        if (!h.ptr)
            return;
        std::lock_guard<std::mutex> guard(m);
        freeList.push_back(h.ptr);
    }
};
CpuDescriptors g_srvCpu, g_rtvCpu, g_dsvCpu, g_samplerCpu;
D3D12_CPU_DESCRIPTOR_HANDLE g_nullSrv, g_defaultSampler;
extern std::atomic<int> g_sceneDumpFrames;
bool MvSequence();
int MvSequenceLevel(); // CRYMT_MVSEQ: 1 every target, 2 only DLSS's input, output and vectors (lighter at large sizes)
std::atomic<bool> g_objectMotion{true}; // the engine's object velocities in DLSS's motion vectors (PgDn with the test keys) // the capture key's DLSS input/output dump (defined with the capture)

// Shader-visible CBV/SRV/UAV descriptors, handed out in submission order (main lists only).
struct DescriptorRing
{
    ID3D12DescriptorHeap* heap = nullptr;
    UINT inc = 0;
    uint64_t size = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0{};
    uint64_t head = 0, tail = 0;
    std::deque<std::pair<uint64_t, uint64_t>> marks;

    void Init(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{type, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        g.dev->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
        inc = g.dev->GetDescriptorHandleIncrementSize(type);
        size = count;
        cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
        gpu0 = heap->GetGPUDescriptorHandleForHeapStart();
    }

    void Reclaim()
    {
        while (!marks.empty() && marks.front().first <= g.completed)
        {
            tail = marks.front().second;
            marks.pop_front();
        }
    }

    void Mark(uint64_t fence)
    {
        if (marks.empty() || marks.back().second != head)
            marks.push_back({fence, head});
    }

    bool TryAlloc(uint32_t n, D3D12_CPU_DESCRIPTOR_HANDLE& c, D3D12_GPU_DESCRIPTOR_HANDLE& gp)
    {
        uint64_t pos = head;
        if (pos % size + n > size)
            pos = (pos / size + 1) * size;
        if (pos + n - tail > size)
            return false;
        head = pos + n;
        c.ptr = cpu0.ptr + static_cast<SIZE_T>(pos % size) * inc;
        gp.ptr = gpu0.ptr + (pos % size) * inc;
        return true;
    }

    uint64_t OldestFence() const
    {
        return marks.empty() ? 0 : marks.front().first;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Root signature: per stage, root CBVs for the constant buffer slots the renderer's shaders use (VS b0-b7, GS b0-b3,
// PS b0-b7), a 16-entry SRV table and a 16-entry sampler table. 46 of 64 DWORDs.
// ---------------------------------------------------------------------------------------------------------------------
enum Stage
{
    kVS,
    kGS,
    kPS,
    kStages
};
constexpr UINT kRootCbSlots[kStages] = {8, 4, 8};
constexpr UINT kRootCb[kStages] = {0, 8, 12};
constexpr UINT kRootSrv[kStages] = {20, 22, 24};
constexpr UINT kRootSampler[kStages] = {21, 23, 25};
constexpr UINT kRootParams = 26;
constexpr UINT kTableSrvs = 16;
constexpr UINT kTableSamplers = 16;

bool CreateRootSignature(SerializeRootSignatureFn serialize)
{
    static const D3D12_SHADER_VISIBILITY kVisibility[kStages] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_GEOMETRY,
                                                                 D3D12_SHADER_VISIBILITY_PIXEL};
    D3D12_ROOT_PARAMETER params[kRootParams]{};
    D3D12_DESCRIPTOR_RANGE ranges[kStages][2]{};
    for (int s = 0; s < kStages; s++)
    {
        for (UINT i = 0; i < kRootCbSlots[s]; i++)
        {
            D3D12_ROOT_PARAMETER& p = params[kRootCb[s] + i];
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p.Descriptor.ShaderRegister = i;
            p.ShaderVisibility = kVisibility[s];
        }
        ranges[s][0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTableSrvs, 0, 0, 0};
        ranges[s][1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kTableSamplers, 0, 0, 0};
        D3D12_ROOT_PARAMETER& srv = params[kRootSrv[s]];
        srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        srv.DescriptorTable = {1, &ranges[s][0]};
        srv.ShaderVisibility = kVisibility[s];
        D3D12_ROOT_PARAMETER& smp = params[kRootSampler[s]];
        smp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        smp.DescriptorTable = {1, &ranges[s][1]};
        smp.ShaderVisibility = kVisibility[s];
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = kRootParams;
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        Log("root signature: serialize failed %08lx %s", static_cast<unsigned long>(hr), error ? static_cast<const char*>(error->GetBufferPointer()) : "");
        return false;
    }
    hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&g.root));
    blob->Release();
    if (FAILED(hr))
        Log("root signature: create failed %08lx", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

// The last texture copies recorded (LogDred prints them): the user's GPU hangs of 2026-11-12 stopped at a
// CopyTextureRegion, twice right before a list's end.
struct CopyRec
{
    uint64_t serial; // main list
    const char* site;
    uint32_t format, width, height, depthOrArray, mips; // the destination
    uint32_t sub, x, y, z;
    uint32_t w, h, d; // the region (source footprint or box)
    uint32_t srcPlaced;
};
CopyRec g_copyRing[64];
uint32_t g_copyNext;

void NoteCopy(const char* site, const D3D12_TEXTURE_COPY_LOCATION& dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION& src,
              const D3D12_BOX* box);
uint64_t g_emptyCopies;      // CopyTexture with an empty box, skipped
uint64_t g_stagingRowCopies; // staging-to-staging copies done as CopyBufferRegion rows (CopyStagingRows)

// After the device was removed: what DRED kept (see the settings at device creation). Unfinished command lists with
// the operations around the last one the GPU completed, then the page fault address and the allocations there.
void LogDred()
{
    static const char* const kOps[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
                                       "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource",
                                       "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView",
                                       "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present",
                                       "ResolveQueryData", "BeginSubmission", "EndSubmission"};
    ID3D12DeviceRemovedExtendedData1* d = nullptr;
    if (!g.dev || FAILED(g.dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1), reinterpret_cast<void**>(&d))))
    {
        Log("DRED: not available");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc{};
    if (SUCCEEDED(d->GetAutoBreadcrumbsOutput1(&bc)))
    {
        int shown = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc.pHeadAutoBreadcrumbNode; n && shown < 8; n = n->pNext)
        {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount)
                continue;
            shown++;
            Log("DRED: list %p (%ls) on queue %ls: %u of %u operations done", static_cast<const void*>(n->pCommandList),
                n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"-", n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"-",
                done, n->BreadcrumbCount);
            UINT draws = 0, dispatches = 0;
            for (UINT i = 0; i < done && n->pCommandHistory; i++)
            {
                draws += n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED || n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED;
                dispatches += n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DISPATCH;
            }
            Log("DRED:   completed before it: %u draws, %u dispatches", draws, dispatches);
            for (UINT i = done > 12 ? done - 12 : 0; n->pCommandHistory && i < n->BreadcrumbCount && i < done + 6; i++)
            {
                const int op = n->pCommandHistory[i];
                Log("DRED:   %s%5u %s", i == done ? "> " : "  ", i, op >= 0 && op < static_cast<int>(std::size(kOps)) ? kOps[op] : "op");
            }
            for (UINT k = 0; k < n->BreadcrumbContextsCount && n->pBreadcrumbContexts; k++)
            {
                const D3D12_DRED_BREADCRUMB_CONTEXT& c = n->pBreadcrumbContexts[k];
                if (c.BreadcrumbIndex + 64 >= done && c.BreadcrumbIndex <= done + 8)
                    Log("DRED:   context at %u: %ls", c.BreadcrumbIndex, c.pContextString ? c.pContextString : L"");
            }
        }
        if (!shown)
            Log("DRED: no unfinished command list recorded (breadcrumbs off?)");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
    if (SUCCEEDED(d->GetPageFaultAllocationOutput1(&pf)))
    {
        Log("DRED: page fault at GPU address %llx", static_cast<unsigned long long>(pf.PageFaultVA));
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* a = pf.pHeadExistingAllocationNode; a && k < 12; a = a->pNext, k++)
            Log("DRED:   allocation there: %ls (type %d)", a->ObjectNameW ? a->ObjectNameW : L"-", static_cast<int>(a->AllocationType));
        k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* a = pf.pHeadRecentFreedAllocationNode; a && k < 12; a = a->pNext, k++)
            Log("DRED:   recently freed there: %ls (type %d)", a->ObjectNameW ? a->ObjectNameW : L"-", static_cast<int>(a->AllocationType));
    }
    d->Release();
    Log("last texture copies (oldest first; destination format WxHxD mips, subresource at x,y,z, region w x h x d):");
    for (uint32_t i = 0; i < 64; i++)
    {
        const CopyRec& c = g_copyRing[(g_copyNext + i) % 64];
        if (!c.site)
            continue;
        Log("  list %llu %-16s fmt %u %ux%ux%u mips %u, sub %u at %u,%u,%u, region %ux%ux%u%s", static_cast<unsigned long long>(c.serial), c.site,
            c.format, c.width, c.height, c.depthOrArray, c.mips, c.sub, c.x, c.y, c.z, c.w, c.h, c.d, c.srcPlaced ? " from a footprint" : "");
    }
}

void NoteCopy(const char* site, const D3D12_TEXTURE_COPY_LOCATION& dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION& src,
              const D3D12_BOX* box)
{
    CopyRec& c = g_copyRing[g_copyNext++ % 64];
    c = {};
    c.serial = g_mainSerial;
    c.site = site;
    if (dst.pResource)
    {
        const D3D12_RESOURCE_DESC rd = dst.pResource->GetDesc();
        c.format = rd.Format;
        c.width = static_cast<uint32_t>(rd.Width);
        c.height = rd.Height;
        c.depthOrArray = rd.DepthOrArraySize;
        c.mips = rd.MipLevels;
    }
    c.sub = dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? dst.SubresourceIndex : ~0u;
    c.x = x;
    c.y = y;
    c.z = z;
    c.srcPlaced = src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    if (box)
    {
        c.w = box->right - box->left;
        c.h = box->bottom - box->top;
        c.d = box->back - box->front;
    }
    else if (c.srcPlaced)
    {
        c.w = src.PlacedFootprint.Footprint.Width;
        c.h = src.PlacedFootprint.Footprint.Height;
        c.d = src.PlacedFootprint.Footprint.Depth;
    }
}

// D3D12 debug layer messages (CRYMT_D3D12DEBUG=1), drained after each submission.
void DrainDebugMessages()
{
    if (!g.info)
        return;
    const UINT64 n = g.info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; i++)
    {
        SIZE_T bytes = 0;
        g.info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> buffer(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (SUCCEEDED(g.info->GetMessage(i, message, &bytes)))
        {
            char key[32];
            snprintf(key, sizeof(key), "dbg%d", message->ID);
            LogLimited(key, "D3D12 [%d] %.*s", message->ID, static_cast<int>(message->DescriptionByteLength), message->pDescription);
        }
    }
    g.info->ClearStoredMessages();
}
// ---------------------------------------------------------------------------------------------------------------------
// Objects. Every D3D10 interface the renderer gets is one of these; the interface is the object's first base, so an
// interface pointer is the object pointer.
// ---------------------------------------------------------------------------------------------------------------------
class Device;
Device* g_device;
// The engine-captured environment cube (Dx12EngineEnvCube, the render thread): its texture, referenced; the stream thread
// makes its view (Device::EngineCube).
std::mutex g_engineCubeLock;
// The engine cube: the D3D slice a render ended in, for each face number its start mark gave (EngineCubeCullFor), and how
// often that changed (the stream thread).
int g_cubeSliceOfMark[6] = {-1, -1, -1, -1, -1, -1};
uint64_t g_cubeSliceLearned;
ID3D10Resource* g_engineCubePending;
std::atomic<bool> g_cubeDumpWanted; // Dx12DumpEngineCube (Num0 with the in-game keys): the cube as one picture at the next present
std::atomic<bool> g_voxSnapWanted;  // Dx12VoxSnap (crymt_voxsnap <name>): the voxel grid's geometry levels to a file
std::mutex g_voxSnapLock;
std::string g_voxSnapName;
std::atomic<float> g_engineCubeTanHalf{1.0f}; // Dx12EngineEnvCubeFov: the engine cube faces' tan(half field of view)
ID3D10Device* DeviceInterface(); // AddRef'd
void SubmitMain();               // context thread: submits the main list being recorded
void PollDone();                 // refreshes the completed serial

std::atomic<uint32_t> g_nextId{1};

// Private data, kept aside (rarely used).
struct PrivateEntry
{
    GUID guid;
    std::vector<uint8_t> data;
    IUnknown* object;
};
std::mutex g_privateLock;
std::unordered_map<const void*, std::vector<PrivateEntry>> g_private;

HRESULT PrivateSet(const void* owner, REFGUID guid, UINT size, const void* data, const IUnknown* object)
{
    std::lock_guard<std::mutex> guard(g_privateLock);
    auto& list = g_private[owner];
    for (auto it = list.begin(); it != list.end(); ++it)
        if (IsEqualGUID(it->guid, guid))
        {
            if (it->object)
                it->object->Release();
            list.erase(it);
            break;
        }
    if (!data && !object)
        return S_OK;
    PrivateEntry e{guid, {}, nullptr};
    if (object)
    {
        e.object = const_cast<IUnknown*>(object);
        e.object->AddRef();
    }
    else
        e.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    list.push_back(std::move(e));
    return S_OK;
}

HRESULT PrivateGet(const void* owner, REFGUID guid, UINT* size, void* data)
{
    if (!size)
        return E_INVALIDARG;
    std::lock_guard<std::mutex> guard(g_privateLock);
    const auto found = g_private.find(owner);
    if (found != g_private.end())
        for (const PrivateEntry& e : found->second)
            if (IsEqualGUID(e.guid, guid))
            {
                const UINT needed = e.object ? static_cast<UINT>(sizeof(IUnknown*)) : static_cast<UINT>(e.data.size());
                if (!data)
                {
                    *size = needed;
                    return S_OK;
                }
                if (*size < needed)
                {
                    *size = needed;
                    return DXGI_ERROR_MORE_DATA;
                }
                if (e.object)
                {
                    e.object->AddRef();
                    *static_cast<IUnknown**>(data) = e.object;
                }
                else if (needed)
                    memcpy(data, e.data.data(), needed);
                *size = needed;
                return S_OK;
            }
    *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}

void PrivateErase(const void* owner)
{
    std::lock_guard<std::mutex> guard(g_privateLock);
    const auto found = g_private.find(owner);
    if (found == g_private.end())
        return;
    for (PrivateEntry& e : found->second)
        if (e.object)
            e.object->Release();
    g_private.erase(found);
}

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

// a * b for D3DX-style 4x4 matrices (row vectors, rows in memory).
void MatMul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

// This view's points (row vectors: x, y, -distance, 1) to the previous frame's clip space, in double, from the two
// views' rotations and the camera's two places (2026-11-13). The float product RigidInverse(view) x previous view lost
// ~0.3 mm of the camera's place 2 km from the world's origin (the view's translation ~3000 has a float step of 0.24 mm,
// and the product cancels it): on a surface 1.5 m away that is 0.2-0.3 px at 5120x1440, the same every frame on a still
// camera (a false zoom) and noise while moving. DLSS kept sliding its history under the textures of near ground and walls
// ("textures flow after the camera moves"; edges, clamped by DLSS, stayed). The place comes from the CCamera (camPos, w
// 1); without it, from the view's translation in double. world = v R^T + c; previous view = world P + t' = v (R^T P) +
// (c - c') P.
void CameraPlace(const float* view, const float* camPos, double* c)
{
    if (camPos && camPos[3] == 1.0f)
    {
        for (int i = 0; i < 3; i++)
            c[i] = camPos[i];
        return;
    }
    for (int i = 0; i < 3; i++)
        c[i] = -(static_cast<double>(view[12]) * view[i * 4] + static_cast<double>(view[13]) * view[i * 4 + 1] + static_cast<double>(view[14]) * view[i * 4 + 2]);
}

void ReprojDouble(const float* view, const float* camPos, const float* prevView, const float* prevCamPos, const float* prevProj, float* out)
{
    double c[3], cp[3];
    CameraPlace(view, camPos, c);
    CameraPlace(prevView, prevCamPos, cp);
    double m[16] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
        {
            double s = 0.0;
            for (int k = 0; k < 3; k++)
                s += static_cast<double>(view[k * 4 + i]) * prevView[k * 4 + j];
            m[i * 4 + j] = s;
        }
    for (int j = 0; j < 3; j++)
    {
        double s = 0.0;
        for (int k = 0; k < 3; k++)
            s += (c[k] - cp[k]) * prevView[k * 4 + j];
        m[12 + j] = s;
    }
    m[15] = 1.0;
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
        {
            double s = 0.0;
            for (int k = 0; k < 4; k++)
                s += m[r * 4 + k] * prevProj[k * 4 + col];
            out[r * 4 + col] = static_cast<float>(s);
        }
}

// The cosine of the camera's turn between two views (their rotations' Frobenius product: trace(R^T R') = 1 + 2 cos).
double ViewTurnCos(const float* view, const float* prevView)
{
    double t = 0.0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            t += static_cast<double>(view[r * 4 + c]) * prevView[r * 4 + c];
    return (t - 1.0) / 2.0;
}

// The camera's move between two frames (metres), for the cut test.
double CameraJump(const float* view, const float* camPos, const float* prevView, const float* prevCamPos)
{
    double c[3], cp[3];
    CameraPlace(view, camPos, c);
    CameraPlace(prevView, prevCamPos, cp);
    return sqrt((c[0] - cp[0]) * (c[0] - cp[0]) + (c[1] - cp[1]) * (c[1] - cp[1]) + (c[2] - cp[2]) * (c[2] - cp[2]));
}

// Inverse of a rotation and translation (the view matrix): transposed rotation, translation moved back through it.
void RigidInverse(const float* m, float* out)
{
    for (int r = 0; r < 3; r++)
    {
        for (int c = 0; c < 3; c++)
            out[r * 4 + c] = m[c * 4 + r];
        out[r * 4 + 3] = 0.0f;
    }
    for (int c = 0; c < 3; c++)
        out[12 + c] = -(m[12] * m[c * 4] + m[13] * m[c * 4 + 1] + m[14] * m[c * 4 + 2]);
    out[15] = 1.0f;
}

// Waits until the GPU is done with a staging resource before the CPU touches it.
HRESULT WaitStaging(ResourceInfo* info, bool read, UINT flags)
{
    const uint64_t serial = read ? info->gpuWriteSerial : std::max(info->gpuWriteSerial, info->gpuReadSerial);
    if (!serial)
        return S_OK;
    PollDone();
    if (SerialDone(serial))
        return S_OK;
    if (serial >= g_mainSerial)
        SubmitMain();
    if (flags & D3D10_MAP_FLAG_DO_NOT_WAIT)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    WaitSerial(serial);
    return S_OK;
}

UINT FormatBytes(DXGI_FORMAT f)
{
    const int v = f;
    if (v >= 1 && v <= 4)
        return 16;
    if (v >= 5 && v <= 8)
        return 12;
    if (v >= 9 && v <= 22)
        return 8;
    if ((v >= 23 && v <= 47) || (v >= 67 && v <= 69) || (v >= 87 && v <= 93))
        return 4;
    if ((v >= 48 && v <= 59) || v == 85 || v == 86)
        return 2;
    if (v >= 60 && v <= 65)
        return 1;
    return 4;
}

class Buffer12 final : public Child<ID3D10Buffer>, public ResourceInfo
{
public:
    D3D10_BUFFER_DESC desc{};
    Block block{}; // default and immutable
    Slice slice{}; // dynamic: the current contents, in stream order (what draws bind)
    Slice prod{};  // dynamic: the producer's current memory (Dx12DiscardNow); equals `slice` once the stream caught up
    struct Retired
    {
        Slice slice;
        uint64_t serial;
    };
    std::deque<Retired> retired;
    uint32_t generation = 0; // dynamic renames, for buffer views
    // Default constant buffers: their first 12 floats as last written (creation, UpdateSubresource), in stream order
    // like the draws - the object matrix (PI_ObjMatrix at c0) the GI geometry's cache keys on.
    float head[12] = {};
    bool headKnown = false;

    Buffer12()
    {
        buffer = true;
    }

    bool Dynamic() const
    {
        return desc.Usage == D3D10_USAGE_DYNAMIC;
    }

    bool Staging() const
    {
        return desc.Usage == D3D10_USAGE_STAGING;
    }

    ID3D12Resource* Res() const
    {
        return Dynamic() ? slice.res : Staging() ? res : block.page ? block.page->res : nullptr;
    }

    uint64_t Offset() const
    {
        return Dynamic() ? slice.offset : Staging() ? 0 : block.offset;
    }

    D3D12_GPU_VIRTUAL_ADDRESS Gpu() const
    {
        return Dynamic() ? slice.gpu : Staging() ? res->GetGPUVirtualAddress() : block.gpu;
    }

    // Default buffers live in a page whose state matters within the main list; null for the others.
    BufferPage* Page() const
    {
        return !Dynamic() && !Staging() ? block.page : nullptr;
    }

    void STDMETHODCALLTYPE GetType(D3D10_RESOURCE_DIMENSION* d) override
    {
        *d = D3D10_RESOURCE_DIMENSION_BUFFER;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}

    UINT STDMETHODCALLTYPE GetEvictionPriority() override
    {
        return 0;
    }

    HRESULT STDMETHODCALLTYPE Map(D3D10_MAP type, UINT flags, void** data) override
    {
        if (!data)
            return E_INVALIDARG;
        if (Dynamic())
        {
            if (type == D3D10_MAP_WRITE_DISCARD)
                Rename();
            else if (type != D3D10_MAP_WRITE_NO_OVERWRITE)
                return E_INVALIDARG;
            *data = slice.cpu;
            return slice.cpu ? S_OK : E_OUTOFMEMORY;
        }
        if (Staging())
        {
            const HRESULT hr = WaitStaging(this, type == D3D10_MAP_READ || type == D3D10_MAP_READ_WRITE, flags);
            if (FAILED(hr))
                return hr;
            *data = stagingCpu;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    void STDMETHODCALLTYPE Unmap() override {}

    void STDMETHODCALLTYPE GetDesc(D3D10_BUFFER_DESC* d) override
    {
        *d = desc;
    }

    // Map(DISCARD): the current slice goes back once the list being recorded has completed.
    void Rename()
    {
        const int kind = (desc.BindFlags & D3D10_BIND_CONSTANT_BUFFER) ? 0 : (desc.BindFlags & (D3D10_BIND_VERTEX_BUFFER | D3D10_BIND_INDEX_BUFFER)) ? 1 : 2;
        g_renames[kind]++;
        g_renameBytes[kind] += desc.ByteWidth;
        retired.push_back({slice, g_mainSerial});
        if (SerialDone(retired.front().serial + SliceMargin()))
        {
            slice = retired.front().slice;
            retired.pop_front();
        }
        else
            slice = g_slices.Alloc(desc.ByteWidth);
        generation++;
    }

    // Stream thread: the memory the producer took at Map(DISCARD) becomes the contents; the old one goes back to the
    // heap once the list being recorded has completed (retired slices the GPU is done with go back now).
    void ApplySlice(const Slice& s)
    {
        const int kind = (desc.BindFlags & D3D10_BIND_CONSTANT_BUFFER) ? 0 : (desc.BindFlags & (D3D10_BIND_VERTEX_BUFFER | D3D10_BIND_INDEX_BUFFER)) ? 1 : 2;
        g_renames[kind]++;
        g_renameBytes[kind] += desc.ByteWidth;
        retired.push_back({slice, g_mainSerial});
        slice = s;
        generation++;
        while (!retired.empty() && SerialDone(retired.front().serial + SliceMargin()))
        {
            g_slices.Free(retired.front().slice);
            retired.pop_front();
        }
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10Resource)) || Child::Supports(riid);
    }

    void Destroy() override
    {
        if (Dynamic())
        {
            std::vector<Slice> slices{slice};
            for (const Retired& r : retired)
                slices.push_back(r.slice);
            // Memory the producer took that the stream never applied is in neither list (without the producer path
            // prod stays the creation slice, which is in one of them).
            bool known = prod.cpu == slice.cpu;
            for (const Retired& r : retired)
                known = known || r.slice.cpu == prod.cpu;
            if (!known)
                slices.push_back(prod);
            DeferCall([slices] {
                for (const Slice& s : slices)
                    g_slices.Free(s);
            });
        }
        else if (Staging())
            DeferRelease(res);
        else
        {
            const Block b = block;
            DeferCall([b] { g_blocks.Free(b); });
        }
        Child::Destroy();
    }
};

// Direct dynamic-buffer maps (see D3D12Backend.h). The buffer's vtable tells ours from others (the stream patches its
// Map/Unmap entries in place, the pointer stays).
const void* g_buffer12Vtable;
uint64_t g_directDiscards, g_directDiscardBytes; // producer side

Buffer12* OurDynamicBuffer(ID3D10Buffer* buffer)
{
    if (!buffer || !g_buffer12Vtable || *reinterpret_cast<void* const*>(buffer) != g_buffer12Vtable)
        return nullptr;
    auto* b = static_cast<Buffer12*>(buffer);
    return b->Dynamic() ? b : nullptr;
}

bool DiscardNow(ID3D10Buffer* buffer, Dx12Discard* out)
{
    Buffer12* b = OurDynamicBuffer(buffer);
    if (!b)
        return false;
    const Slice s = g_slices.Alloc(b->desc.ByteWidth);
    out->cpu = s.cpu;
    if (!s.cpu)
        return true;
    b->prod = s;
    out->words[0] = reinterpret_cast<uint64_t>(s.res);
    out->words[1] = s.offset;
    out->words[2] = reinterpret_cast<uint64_t>(s.cpu);
    out->words[3] = s.gpu;
    out->words[4] = static_cast<uint64_t>(static_cast<int64_t>(s.cls));
    g_directDiscards++;
    g_directDiscardBytes += b->desc.ByteWidth;
    return true;
}

void* ProducerMemory(ID3D10Buffer* buffer)
{
    Buffer12* b = OurDynamicBuffer(buffer);
    return b ? b->prod.cpu : nullptr;
}

void ApplyDiscard(ID3D10Buffer* buffer, const uint64_t* w)
{
    Slice s;
    s.res = reinterpret_cast<ID3D12Resource*>(w[0]);
    s.offset = w[1];
    s.cpu = reinterpret_cast<uint8_t*>(w[2]);
    s.gpu = w[3];
    s.cls = static_cast<int>(static_cast<int64_t>(w[4]));
    static_cast<Buffer12*>(buffer)->ApplySlice(s);
}

void DirectMapStats(uint64_t* discards, uint64_t* bytes)
{
    *discards = g_directDiscards;
    *bytes = g_directDiscardBytes;
}

// The format a depth buffer's views use, for a depth resource described typed or typeless.
DXGI_FORMAT DepthViewFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
        return f;
    }
}

bool TypelessColor(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return true;
    default:
        return false;
    }
}

// Textures of all dimensions share this; the interfaces differ only in Map's signature and the description type.
struct TextureCore : ResourceInfo
{
    D3D10_RESOURCE_DIMENSION dim{};
    D3D10_USAGE usage{};
    UINT bind = 0, cpuAccess = 0, misc = 0;
    DXGI_FORMAT format{};
    UINT width = 1, height = 1, depthOrArray = 1, mips = 1, samples = 1, quality = 0;
    UINT subresources = 1;
    // Staging: a buffer holding every subresource at its copyable footprint.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    // Dynamic: Map(DISCARD) writes to upload memory, Unmap records the copy.
    struct Pending
    {
        UploadAlloc up;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        bool active;
    };
    std::vector<Pending> pending;

    bool Staging() const
    {
        return usage == D3D10_USAGE_STAGING;
    }

    D3D12_RESOURCE_DESC Desc12() const
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D   ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                      : dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                  : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = static_cast<UINT16>(depthOrArray);
        d.MipLevels = static_cast<UINT16>(mips);
        d.Format = format;
        d.SampleDesc = {samples, quality};
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (bind & D3D10_BIND_RENDER_TARGET)
            d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (bind & D3D10_BIND_DEPTH_STENCIL)
        {
            // Depth buffers are typeless so they can also be read (DLSS reads the scene depth); views keep the
            // renderer's formats. (Measured: typeless depth costs no GPU time.)
            d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            switch (format)
            {
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
                d.Format = DXGI_FORMAT_R24G8_TYPELESS;
                break;
            case DXGI_FORMAT_D32_FLOAT:
                d.Format = DXGI_FORMAT_R32_TYPELESS;
                break;
            case DXGI_FORMAT_D16_UNORM:
                d.Format = DXGI_FORMAT_R16_TYPELESS;
                break;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
                break;
            default:
                break;
            }
        }
        return d;
    }

    HRESULT MapSub(UINT sub, D3D10_MAP type, UINT flags, void** data, UINT* rowPitch, UINT* depthPitch);
    void UnmapSub(UINT sub);

    void FreeMemory()
    {
        DeferRelease(res);
        res = nullptr;
    }
};

template <class I, class Desc, D3D10_RESOURCE_DIMENSION Dim>
class Texture12 : public Child<I>, public TextureCore
{
public:
    Desc desc{};

    void STDMETHODCALLTYPE GetType(D3D10_RESOURCE_DIMENSION* d) override
    {
        *d = Dim;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}

    UINT STDMETHODCALLTYPE GetEvictionPriority() override
    {
        return 0;
    }

    void STDMETHODCALLTYPE GetDesc(Desc* d) override
    {
        *d = desc;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10Resource)) || Child<I>::Supports(riid);
    }

    void Destroy() override
    {
        FreeMemory();
        Child<I>::Destroy();
    }
};

class Texture1D12 final : public Texture12<ID3D10Texture1D, D3D10_TEXTURE1D_DESC, D3D10_RESOURCE_DIMENSION_TEXTURE1D>
{
public:
    HRESULT STDMETHODCALLTYPE Map(UINT sub, D3D10_MAP type, UINT flags, void** data) override
    {
        return MapSub(sub, type, flags, data, nullptr, nullptr);
    }

    void STDMETHODCALLTYPE Unmap(UINT sub) override
    {
        UnmapSub(sub);
    }
};

class Texture2D12 final : public Texture12<ID3D10Texture2D, D3D10_TEXTURE2D_DESC, D3D10_RESOURCE_DIMENSION_TEXTURE2D>
{
public:
    HRESULT STDMETHODCALLTYPE Map(UINT sub, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE2D* m) override
    {
        if (!m)
            return E_INVALIDARG;
        return MapSub(sub, type, flags, &m->pData, &m->RowPitch, nullptr);
    }

    void STDMETHODCALLTYPE Unmap(UINT sub) override
    {
        UnmapSub(sub);
    }
};

class Texture3D12 final : public Texture12<ID3D10Texture3D, D3D10_TEXTURE3D_DESC, D3D10_RESOURCE_DIMENSION_TEXTURE3D>
{
public:
    HRESULT STDMETHODCALLTYPE Map(UINT sub, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE3D* m) override
    {
        if (!m)
            return E_INVALIDARG;
        return MapSub(sub, type, flags, &m->pData, &m->RowPitch, &m->DepthPitch);
    }

    void STDMETHODCALLTYPE Unmap(UINT sub) override
    {
        UnmapSub(sub);
    }
};

ResourceInfo* Info(ID3D10Resource* r)
{
    if (!r)
        return nullptr;
    D3D10_RESOURCE_DIMENSION d;
    r->GetType(&d);
    switch (d)
    {
    case D3D10_RESOURCE_DIMENSION_BUFFER:
        return static_cast<Buffer12*>(static_cast<ID3D10Buffer*>(r));
    case D3D10_RESOURCE_DIMENSION_TEXTURE1D:
        return static_cast<Texture1D12*>(static_cast<ID3D10Texture1D*>(r));
    case D3D10_RESOURCE_DIMENSION_TEXTURE2D:
        return static_cast<Texture2D12*>(static_cast<ID3D10Texture2D*>(r));
    case D3D10_RESOURCE_DIMENSION_TEXTURE3D:
        return static_cast<Texture3D12*>(static_cast<ID3D10Texture3D*>(r));
    default:
        return nullptr;
    }
}

inline Buffer12* AsBuffer(ID3D10Resource* r)
{
    D3D10_RESOURCE_DIMENSION d;
    r->GetType(&d);
    return d == D3D10_RESOURCE_DIMENSION_BUFFER ? static_cast<Buffer12*>(static_cast<ID3D10Buffer*>(r)) : nullptr;
}

inline TextureCore* AsTexture(ID3D10Resource* r)
{
    ResourceInfo* info = Info(r);
    return info && !info->buffer ? static_cast<TextureCore*>(info) : nullptr;
}

// Views keep their resource alive and own a CPU descriptor.
template <class I>
class View12 : public Child<I>
{
public:
    ID3D10Resource* resource = nullptr;
    ResourceInfo* info = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    CpuDescriptors* heap = nullptr;

    void STDMETHODCALLTYPE GetResource(ID3D10Resource** out) override
    {
        resource->AddRef();
        *out = resource;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10View)) || Child<I>::Supports(riid);
    }

    void Destroy() override
    {
        heap->Free(cpu);
        resource->Release();
        Child<I>::Destroy();
    }
};

class Srv12 final : public View12<ID3D10ShaderResourceView1>
{
public:
    D3D10_SHADER_RESOURCE_VIEW_DESC1 desc{};
    D3D12_SHADER_RESOURCE_VIEW_DESC d12{};
    uint32_t generation = 0; // dynamic buffer views: the rename the descriptor was written for

    void STDMETHODCALLTYPE GetDesc(D3D10_SHADER_RESOURCE_VIEW_DESC* d) override
    {
        memcpy(d, &desc, sizeof(*d));
    }

    void STDMETHODCALLTYPE GetDesc1(D3D10_SHADER_RESOURCE_VIEW_DESC1* d) override
    {
        *d = desc;
    }

    // Buffer views of dynamic buffers follow the buffer's renames.
    void Refresh()
    {
        if (!info->buffer)
            return;
        auto* b = static_cast<Buffer12*>(info);
        if (!b->Dynamic() || generation == b->generation)
            return;
        generation = b->generation;
        D3D12_SHADER_RESOURCE_VIEW_DESC d = d12;
        d.Buffer.FirstElement += b->Offset() / FormatBytes(d.Format);
        g.dev->CreateShaderResourceView(b->Res(), &d, cpu);
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10ShaderResourceView)) || View12::Supports(riid);
    }
};

class Rtv12 final : public View12<ID3D10RenderTargetView>
{
public:
    D3D10_RENDER_TARGET_VIEW_DESC desc{};
    DXGI_FORMAT format{};
    UINT samples = 1, quality = 0;

    void STDMETHODCALLTYPE GetDesc(D3D10_RENDER_TARGET_VIEW_DESC* d) override
    {
        *d = desc;
    }
};

class Dsv12 final : public View12<ID3D10DepthStencilView>
{
public:
    D3D10_DEPTH_STENCIL_VIEW_DESC desc{};
    DXGI_FORMAT format{};
    UINT samples = 1, quality = 0;

    void STDMETHODCALLTYPE GetDesc(D3D10_DEPTH_STENCIL_VIEW_DESC* d) override
    {
        *d = desc;
    }
};

// State objects are shared by description, as in D3D10 (the same object is returned for the same description).
std::mutex g_stateLock;

template <class T>
struct StateCache
{
    std::unordered_map<std::string, T*> map;

    T* Find(const std::string& key) // under g_stateLock; returns a new reference
    {
        const auto it = map.find(key);
        return it != map.end() && it->second->TryAddRef() ? it->second : nullptr;
    }

    void Erase(const std::string& key, T* object) // under g_stateLock
    {
        const auto it = map.find(key);
        if (it != map.end() && it->second == object)
            map.erase(it);
    }
};

template <class I, class Self>
class State12 : public Child<I>
{
public:
    std::string key;
    StateCache<Self>* cache = nullptr;

protected:
    void Destroy() override
    {
        {
            std::lock_guard<std::mutex> guard(g_stateLock);
            cache->Erase(key, static_cast<Self*>(this));
        }
        Child<I>::Destroy();
    }
};

class Blend12 final : public State12<ID3D10BlendState1, Blend12>
{
public:
    D3D10_BLEND_DESC1 desc{};
    D3D12_BLEND_DESC d12{};

    void STDMETHODCALLTYPE GetDesc(D3D10_BLEND_DESC* d) override
    {
        d->AlphaToCoverageEnable = desc.AlphaToCoverageEnable;
        const D3D10_RENDER_TARGET_BLEND_DESC1& rt = desc.RenderTarget[0];
        d->SrcBlend = rt.SrcBlend;
        d->DestBlend = rt.DestBlend;
        d->BlendOp = rt.BlendOp;
        d->SrcBlendAlpha = rt.SrcBlendAlpha;
        d->DestBlendAlpha = rt.DestBlendAlpha;
        d->BlendOpAlpha = rt.BlendOpAlpha;
        for (int i = 0; i < 8; i++)
        {
            d->BlendEnable[i] = desc.RenderTarget[i].BlendEnable;
            d->RenderTargetWriteMask[i] = desc.RenderTarget[i].RenderTargetWriteMask;
        }
    }

    void STDMETHODCALLTYPE GetDesc1(D3D10_BLEND_DESC1* d) override
    {
        *d = desc;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10BlendState)) || State12::Supports(riid);
    }
};

class Depth12 final : public State12<ID3D10DepthStencilState, Depth12>
{
public:
    D3D10_DEPTH_STENCIL_DESC desc{};
    D3D12_DEPTH_STENCIL_DESC d12{};

    void STDMETHODCALLTYPE GetDesc(D3D10_DEPTH_STENCIL_DESC* d) override
    {
        *d = desc;
    }
};

class Raster12 final : public State12<ID3D10RasterizerState, Raster12>
{
public:
    D3D10_RASTERIZER_DESC desc{};
    D3D12_RASTERIZER_DESC d12{};

    void STDMETHODCALLTYPE GetDesc(D3D10_RASTERIZER_DESC* d) override
    {
        *d = desc;
    }
};

class Sampler12 final : public State12<ID3D10SamplerState, Sampler12>
{
public:
    D3D10_SAMPLER_DESC desc{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};

    void STDMETHODCALLTYPE GetDesc(D3D10_SAMPLER_DESC* d) override
    {
        *d = desc;
    }

protected:
    void Destroy() override
    {
        g_samplerCpu.Free(cpu);
        State12::Destroy();
    }
};

StateCache<Blend12> g_blendCache;
StateCache<Depth12> g_depthCache;
StateCache<Raster12> g_rasterCache;
StateCache<Sampler12> g_samplerCache;

class Layout12 final : public Child<ID3D10InputLayout>
{
public:
    uint64_t contentHash = 0;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::deque<std::string> names; // stable storage for the semantic names
    UINT slotEnd = 0;              // highest input slot + 1
    bool animated = false;         // bone weights or indices (skinned), or a per-instance stream: the GI geometry
                                   // voxelizes its draws every time (RunVoxelGeometry)
};

// A fast 64-bit hash, 8 bytes a step (GI geometry keys: ~2300 draws a frame; byte-wise FNV and the CRT's lrintf
// there cost ~1.3 ms a frame on the stream thread, the sampler showed).
uint64_t HashWords(const void* data, size_t bytes, uint64_t h)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (; bytes >= 8; bytes -= 8, p += 8)
    {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; bytes; bytes--, p++)
        h = (h ^ *p) * 0x100000001B3ull;
    return h;
}

// Shaders keep their bytecode for pipeline state creation and what reflection says they bind.
uint64_t HashBytes(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

template <class I>
class Shader12 final : public Child<I>
{
public:
    uint64_t contentHash = 0; // for pipeline library names, stable across runs
    std::vector<uint8_t> code;
    uint32_t cbMask = 0; // constant buffer slots
    UINT srvEnd = 0;     // highest texture slot + 1
    UINT samplerEnd = 0; // highest sampler slot + 1
    bool motionBlur = false; // the object motion blur pass (its previous object matrix, MotionBlurData, in the reflection data)
    // For the GI geometry pass: a vertex shader's outputs (a pixel shader reading its texture coordinates must declare
    // them alike), a pixel shader's lowest 2D texture slot and whether it discards (alpha test).
    struct Output
    {
        std::string name;
        UINT index, reg;
        BYTE mask;
        D3D10_REGISTER_COMPONENT_TYPE type;
        D3D10_NAME value;
    };
    std::vector<Output> outputs;
    int firstTexture = -1;
    int diffuseSlot = -1; // the texture diffuseMapSampler (the z-prepass's alpha test reads its alpha), -1 none
    bool refractive = false; // water, glass, ice, liquids (no CryMT reflection): see IsRefractiveResource
    bool noEnv = false;      // eyes, hair, skin (Eye.cfx, Hair.cfx, HumanSkin.cfx by their own parameters): no CryMT reflection
    int envCubeSlot = -1, env2DSlot = -1; // envMapSamplerCUBE ($EnvironmentCubeMap) / envMapSampler ($Env): the material's env map
    int waterEnvSamplerSlot = -1;         // Water.cfx's CryMT_WaterEnvSampler (its sampler state: the back end's trilinear clamp)
    bool water = false; // a vertex shader with the constant WaterLevel (WaterOcean.cfx, its z-prepass too): not voxel geometry
    bool discards = false;
    bool writesTarget1 = false; // a pixel shader writing SV_Target1 (the patched z-prepass: the foliage mask)
    bool writesTarget2 = false; // ... SV_Target2 (the patched z-prepass: wind-bent vegetation's screen move)
    bool particleLayer = false; // ... a one-component SV_Target2: the patched particles' layer (BindParticleLayer)
    bool plainCopy = false;     // a pixel shader that only samples one texture into its output (IsPlainCopy)
    // The engine's fog constants where the shader declares them (constant buffer slot, byte offset; -1 none): vfParams,
    // vfFogColor, slHazeColPartialMieInScatter, slHazeColPartialRayleighInScatter (RunVfog reads them in the fog pass).
    int fogSlot[4] = {-1, -1, -1, -1};
    UINT fogOffset[4] = {};
    int vfogSlot = -1;      // the patched fog pass's CryMT_VFogSampler texture slot (RunVfog's volume goes there)
    int vfogParamSlot = -1; // ... its CryMT_VFogParamSampler slot (the parameters, with the mark)

    // SM4 token stream (the SHDR chunk): opcode in bits 0-10, length in 24-30 (custom data: the next token).
    static bool HasDiscard(const std::vector<uint8_t>& dxbc)
    {
        if (dxbc.size() < 32 || memcmp(dxbc.data(), "DXBC", 4) != 0)
            return false;
        const uint32_t chunks = *reinterpret_cast<const uint32_t*>(&dxbc[28]);
        for (uint32_t c = 0; c < chunks && 32 + c * 4 + 4 <= dxbc.size(); c++)
        {
            const uint32_t at = *reinterpret_cast<const uint32_t*>(&dxbc[32 + c * 4]);
            if (at + 8 > dxbc.size() || (memcmp(&dxbc[at], "SHDR", 4) != 0 && memcmp(&dxbc[at], "SHEX", 4) != 0))
                continue;
            const uint32_t bytes = *reinterpret_cast<const uint32_t*>(&dxbc[at + 4]);
            const uint32_t* t = reinterpret_cast<const uint32_t*>(&dxbc[at + 8]);
            const size_t n = std::min<size_t>(bytes, dxbc.size() - at - 8) / 4;
            for (size_t i = 2; i < n;)
            {
                const uint32_t op = t[i] & 0x7FF;
                if (op == 13) // discard
                    return true;
                const uint32_t len = op == 0x35 && i + 1 < n ? t[i + 1] : (t[i] >> 24) & 0x7F;
                i += len ? len : 1;
            }
        }
        return false;
    }

    // A pixel shader that only copies a texture: declarations, one sample, ret (SM4 opcodes: 0x45 sample, 0x3E ret, 0x35
    // custom data, 0x58-0x6A declarations). The engine copies its jittered scene with one (12c069f424600b76) before DLSS.
    static bool IsPlainCopy(const std::vector<uint8_t>& dxbc)
    {
        if (dxbc.size() < 32 || memcmp(dxbc.data(), "DXBC", 4) != 0)
            return false;
        const uint32_t chunks = *reinterpret_cast<const uint32_t*>(&dxbc[28]);
        for (uint32_t c = 0; c < chunks && 32 + c * 4 + 4 <= dxbc.size(); c++)
        {
            const uint32_t at = *reinterpret_cast<const uint32_t*>(&dxbc[32 + c * 4]);
            if (at + 8 > dxbc.size() || (memcmp(&dxbc[at], "SHDR", 4) != 0 && memcmp(&dxbc[at], "SHEX", 4) != 0))
                continue;
            const uint32_t bytes = *reinterpret_cast<const uint32_t*>(&dxbc[at + 4]);
            const uint32_t* t = reinterpret_cast<const uint32_t*>(&dxbc[at + 8]);
            const size_t n = std::min<size_t>(bytes, dxbc.size() - at - 8) / 4;
            int samples = 0;
            for (size_t i = 2; i < n;)
            {
                const uint32_t op = t[i] & 0x7FF;
                if (op == 0x45)
                    samples++;
                else if (op != 0x3E && op != 0x35 && (op < 0x58 || op > 0x6A))
                    return false;
                const uint32_t len = op == 0x35 && i + 1 < n ? t[i + 1] : (t[i] >> 24) & 0x7F;
                i += len ? len : 1;
            }
            return samples == 1;
        }
        return false;
    }

    void Reflect()
    {
        discards = HasDiscard(code);
        plainCopy = IsPlainCopy(code);
        static const char kName[] = "MotionBlurData";
        motionBlur = std::search(code.begin(), code.end(), kName, kName + sizeof(kName) - 1) != code.end();
        ID3D10ShaderReflection* r = nullptr;
        if (!g.reflect || FAILED(g.reflect(code.data(), code.size(), &r)) || !r)
        {
            cbMask = 0xFFFF;
            srvEnd = kTableSrvs;
            samplerEnd = kTableSamplers;
            return;
        }
        D3D10_SHADER_DESC d{};
        r->GetDesc(&d);
        std::string resourceNames; // for the environment map log below
        for (UINT i = 0; i < d.BoundResources; i++)
        {
            D3D10_SHADER_INPUT_BIND_DESC b{};
            if (FAILED(r->GetResourceBindingDesc(i, &b)))
                continue;
            const UINT end = b.BindPoint + b.BindCount;
            if (b.Type == D3D10_SIT_TEXTURE && b.Dimension == D3D10_SRV_DIMENSION_TEXTURE2D &&
                (firstTexture < 0 || static_cast<int>(b.BindPoint) < firstTexture))
                firstTexture = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "diffuseMapSampler") == 0)
                diffuseSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && IsRefractiveResource(b.Name))
                refractive = true;
            if (b.Name && resourceNames.size() < 600)
                resourceNames += std::string(resourceNames.empty() ? "" : " ") + b.Name;
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "envMapSamplerCUBE") == 0)
                envCubeSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "envMapSampler") == 0)
                env2DSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_SAMPLER && b.Name && strcmp(b.Name, "CryMT_WaterEnvSampler") == 0)
                waterEnvSamplerSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "CryMT_VFogSampler") == 0)
                vfogSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "CryMT_VFogParamSampler") == 0)
                vfogParamSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_CBUFFER)
                cbMask |= ((1u << b.BindCount) - 1) << b.BindPoint;
            else if (b.Type == D3D10_SIT_SAMPLER)
                samplerEnd = std::max(samplerEnd, end);
            else
                srvEnd = std::max(srvEnd, end);
        }
        // Eyes, hair and skin keep their own look (the user, 2026-11-13): told by parameters only their shaders have.
        static const char* const kNoEnvParams[] = {"IrisColor", "CorneaOpacity", "IrisGlintMul", "HairColor", "PrimarySpecShift", "SecondarySpecShift",
                                                   "PrimarySpecColor", "Melanin", "SubsurfaceStrenght", "DiffusionAmount", "WrinklesBlend"};
        for (UINT i = 0; i < d.ConstantBuffers && !noEnv; i++)
            if (ID3D10ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i))
            {
                D3D10_SHADER_BUFFER_DESC bd{};
                if (SUCCEEDED(cb->GetDesc(&bd)))
                    for (UINT j = 0; j < bd.Variables && !noEnv; j++)
                    {
                        D3D10_SHADER_VARIABLE_DESC vd{};
                        ID3D10ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
                        if (var && SUCCEEDED(var->GetDesc(&vd)) && vd.Name)
                            for (const char* p : kNoEnvParams)
                                noEnv = noEnv || strcmp(vd.Name, p) == 0;
                    }
            }
        for (UINT i = 0; i < d.ConstantBuffers && !water; i++)
            if (ID3D10ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i))
            {
                D3D10_SHADER_BUFFER_DESC bd{};
                if (SUCCEEDED(cb->GetDesc(&bd)))
                    for (UINT j = 0; j < bd.Variables && !water; j++)
                    {
                        D3D10_SHADER_VARIABLE_DESC vd{};
                        ID3D10ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
                        water = var && SUCCEEDED(var->GetDesc(&vd)) && vd.Name && strcmp(vd.Name, "WaterLevel") == 0;
                    }
            }
        if (water)
        {
            static int logged;
            if (logged++ < 4)
                Log("ocean vertex shader recognized (WaterLevel), %u bytes", static_cast<unsigned>(code.size()));
        }
        if (vfogSlot >= 0 || vfogParamSlot >= 0)
        {
            static int logged;
            if (logged++ < 6)
                Log("fog pass shader with the GI in the fog: volume t%d, parameters t%d", vfogSlot, vfogParamSlot);
        }
        if (envCubeSlot >= 0 || env2DSlot >= 0)
        {
            static int kept, replaced;
            if (refractive ? kept++ < 24 : replaced++ < 8)
                Log("pixel shader with an environment map %s: %s", refractive ? "keeps it (water, glass)" : "may take CryMT's", resourceNames.c_str());
        }
        for (UINT i = 0; i < d.OutputParameters; i++)
        {
            D3D10_SIGNATURE_PARAMETER_DESC o{};
            if (SUCCEEDED(r->GetOutputParameterDesc(i, &o)) && o.SemanticName)
            {
                outputs.push_back({o.SemanticName, o.SemanticIndex, o.Register, o.Mask, o.ComponentType, o.SystemValueType});
                if (_stricmp(o.SemanticName, "SV_Target") == 0 && o.SemanticIndex == 1) // reflected with system value 0
                    writesTarget1 = true;
                if (_stricmp(o.SemanticName, "SV_Target") == 0 && o.SemanticIndex == 2)
                {
                    writesTarget2 = true;
                    particleLayer = o.Mask == 0x1;
                }
            }
        }
        // The fog's constants (fogSlot): the buffer's slot from its binding by name.
        static const char* const kFogVars[4] = {"vfParams", "vfFogColor", "slHazeColPartialMieInScatter", "slHazeColPartialRayleighInScatter"};
        for (UINT i = 0; i < d.ConstantBuffers; i++)
            if (ID3D10ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i))
            {
                D3D10_SHADER_BUFFER_DESC bd{};
                if (FAILED(cb->GetDesc(&bd)) || !bd.Name)
                    continue;
                int slot = -1;
                for (UINT b = 0; b < d.BoundResources && slot < 0; b++)
                {
                    D3D10_SHADER_INPUT_BIND_DESC bind{};
                    if (SUCCEEDED(r->GetResourceBindingDesc(b, &bind)) && bind.Type == D3D10_SIT_CBUFFER && bind.Name && strcmp(bind.Name, bd.Name) == 0)
                        slot = static_cast<int>(bind.BindPoint);
                }
                for (UINT j = 0; j < bd.Variables && slot >= 0; j++)
                {
                    D3D10_SHADER_VARIABLE_DESC vd{};
                    ID3D10ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
                    if (var && SUCCEEDED(var->GetDesc(&vd)) && vd.Name)
                        for (int k = 0; k < 4; k++)
                            if (strcmp(vd.Name, kFogVars[k]) == 0)
                            {
                                fogSlot[k] = slot;
                                fogOffset[k] = vd.StartOffset;
                            }
                }
            }
        r->Release();
    }
};
using VS12 = Shader12<ID3D10VertexShader>;
using GS12 = Shader12<ID3D10GeometryShader>;
using PS12 = Shader12<ID3D10PixelShader>;

class Query12 final : public Child<ID3D10Query>
{
public:
    D3D10_QUERY_DESC desc{};
    int heapIndex = -1;     // occlusion and timestamp queries
    uint64_t serial = 0;    // main list that ends it
    bool ended = false;
    bool split = false;     // an occlusion count that spans a submission: the first part is in the partial slot

    void STDMETHODCALLTYPE Begin() override;
    void STDMETHODCALLTYPE End() override;
    HRESULT STDMETHODCALLTYPE GetData(void* data, UINT size, UINT flags) override;

    UINT STDMETHODCALLTYPE GetDataSize() override
    {
        switch (desc.Query)
        {
        case D3D10_QUERY_EVENT:
        case D3D10_QUERY_OCCLUSION_PREDICATE:
            return sizeof(BOOL);
        case D3D10_QUERY_TIMESTAMP_DISJOINT:
            return sizeof(D3D10_QUERY_DATA_TIMESTAMP_DISJOINT);
        default:
            return sizeof(UINT64);
        }
    }

    void STDMETHODCALLTYPE GetDesc(D3D10_QUERY_DESC* d) override
    {
        *d = desc;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10Asynchronous)) || Child::Supports(riid);
    }

    void Destroy() override;
};
// ---------------------------------------------------------------------------------------------------------------------
// Creation uploads: their own command list, executed before the next main list (or alone when the ring runs out).
// ---------------------------------------------------------------------------------------------------------------------
AllocatorPool g_uploadAllocs;
ID3D12GraphicsCommandList* g_uploadList;
ID3D12CommandAllocator* g_uploadAlloc;
bool g_uploadOpen;
UploadRing g_initRing;
uint64_t g_uploadBytes, g_uploadSubmits;

ID3D12GraphicsCommandList* UploadList() // under g.lock
{
    if (!g_uploadOpen)
    {
        g_uploadAlloc = g_uploadAllocs.Get();
        if (!g_uploadList)
            g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_uploadAlloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                                     reinterpret_cast<void**>(&g_uploadList));
        else
            g_uploadList->Reset(g_uploadAlloc, nullptr);
        g_uploadOpen = true;
    }
    return g_uploadList;
}

void ExecuteUploads() // under g.lock
{
    if (!g_uploadOpen)
        return;
    g_uploadList->Close();
    ID3D12CommandList* lists[] = {g_uploadList};
    g.queue->ExecuteCommandLists(1, lists);
    const uint64_t fence = SignalQueue();
    g_uploadAllocs.Put(g_uploadAlloc, fence);
    g_initRing.Mark(fence);
    g_uploadOpen = false;
    g_uploadSubmits++;
}

UploadAlloc InitUpload(uint64_t bytes, uint64_t align) // under g.lock
{
    g_uploadBytes += bytes;
    if (bytes > g_initRing.size / 4)
    {
        UploadAlloc a;
        a.res = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (a.res)
        {
            a.cpu = MapWhole(a.res);
            DeferRelease(a.res);
        }
        return a;
    }
    for (;;)
    {
        PollFence();
        g_initRing.Reclaim();
        UploadAlloc a;
        if (g_initRing.TryAlloc(bytes, align, a))
            return a;
        ExecuteUploads();
        WaitFence(g_initRing.OldestFence() ? g_initRing.OldestFence() : g.signaled);
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

bool BlockCompressed(DXGI_FORMAT f)
{
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

UINT BlockBytes(DXGI_FORMAT f)
{
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) || (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM) ? 8 : 16;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipeline state and sampler table keys.
// ---------------------------------------------------------------------------------------------------------------------
struct PsoKey
{
    uint32_t vs, gs, ps, layout, blend, depth, raster, sampleMask;
    DXGI_FORMAT rtv[8];
    DXGI_FORMAT dsv;
    uint8_t numRtv, topologyType, stripCut, samples, quality, pad[3];

    bool operator==(const PsoKey& o) const
    {
        return memcmp(this, &o, sizeof(*this)) == 0;
    }
};

struct SamplerKey
{
    uint32_t ids[kTableSamplers];
    uint32_t count;

    bool operator==(const SamplerKey& o) const
    {
        return memcmp(this, &o, sizeof(*this)) == 0;
    }
};

struct BytesHash
{
    template <class T>
    size_t operator()(const T& k) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&k);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(T); i++)
            h = (h ^ p[i]) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(D3D10_PRIMITIVE_TOPOLOGY t)
{
    switch (t)
    {
    case D3D10_PRIMITIVE_TOPOLOGY_POINTLIST:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case D3D10_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D10_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    case D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

bool StripTopology(D3D10_PRIMITIVE_TOPOLOGY t)
{
    return t == D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP || t == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP || t == D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
           t == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
}

D3D12_BLEND_DESC Blend12From(const D3D10_BLEND_DESC1& d)
{
    D3D12_BLEND_DESC b{};
    b.AlphaToCoverageEnable = d.AlphaToCoverageEnable;
    b.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; i++)
    {
        const D3D10_RENDER_TARGET_BLEND_DESC1& s = d.RenderTarget[d.IndependentBlendEnable ? i : 0];
        D3D12_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[i];
        t.BlendEnable = d.IndependentBlendEnable ? s.BlendEnable : d.RenderTarget[i].BlendEnable;
        t.SrcBlend = static_cast<D3D12_BLEND>(s.SrcBlend);
        t.DestBlend = static_cast<D3D12_BLEND>(s.DestBlend);
        t.BlendOp = static_cast<D3D12_BLEND_OP>(s.BlendOp);
        t.SrcBlendAlpha = static_cast<D3D12_BLEND>(s.SrcBlendAlpha);
        t.DestBlendAlpha = static_cast<D3D12_BLEND>(s.DestBlendAlpha);
        t.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(s.BlendOpAlpha);
        t.LogicOp = D3D12_LOGIC_OP_NOOP;
        t.RenderTargetWriteMask = d.IndependentBlendEnable ? s.RenderTargetWriteMask : d.RenderTarget[i].RenderTargetWriteMask;
    }
    return b;
}

D3D10_BLEND_DESC1 Blend1From(const D3D10_BLEND_DESC& d)
{
    D3D10_BLEND_DESC1 b{};
    b.AlphaToCoverageEnable = d.AlphaToCoverageEnable;
    b.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; i++)
        b.RenderTarget[i] = {d.BlendEnable[i], d.SrcBlend, d.DestBlend, d.BlendOp, d.SrcBlendAlpha, d.DestBlendAlpha, d.BlendOpAlpha,
                             d.RenderTargetWriteMask[i]};
    return b;
}

D3D12_DEPTH_STENCIL_DESC Depth12From(const D3D10_DEPTH_STENCIL_DESC& d)
{
    D3D12_DEPTH_STENCIL_DESC r{};
    r.DepthEnable = d.DepthEnable;
    r.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(d.DepthWriteMask);
    r.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(d.DepthFunc);
    r.StencilEnable = d.StencilEnable;
    r.StencilReadMask = d.StencilReadMask;
    r.StencilWriteMask = d.StencilWriteMask;
    auto face = [](const D3D10_DEPTH_STENCILOP_DESC& f) {
        return D3D12_DEPTH_STENCILOP_DESC{static_cast<D3D12_STENCIL_OP>(f.StencilFailOp), static_cast<D3D12_STENCIL_OP>(f.StencilDepthFailOp),
                                          static_cast<D3D12_STENCIL_OP>(f.StencilPassOp), static_cast<D3D12_COMPARISON_FUNC>(f.StencilFunc)};
    };
    r.FrontFace = face(d.FrontFace);
    r.BackFace = face(d.BackFace);
    return r;
}

D3D12_RASTERIZER_DESC Raster12From(const D3D10_RASTERIZER_DESC& d)
{
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = static_cast<D3D12_FILL_MODE>(d.FillMode);
    r.CullMode = static_cast<D3D12_CULL_MODE>(d.CullMode);
    r.FrontCounterClockwise = d.FrontCounterClockwise;
    r.DepthBias = d.DepthBias;
    r.DepthBiasClamp = d.DepthBiasClamp;
    r.SlopeScaledDepthBias = d.SlopeScaledDepthBias;
    r.DepthClipEnable = d.DepthClipEnable;
    r.MultisampleEnable = d.MultisampleEnable;
    r.AntialiasedLineEnable = d.AntialiasedLineEnable;
    r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return r;
}

const D3D10_BLEND_DESC1 kDefaultBlend = Blend1From({FALSE, {}, D3D10_BLEND_ONE, D3D10_BLEND_ZERO, D3D10_BLEND_OP_ADD, D3D10_BLEND_ONE, D3D10_BLEND_ZERO,
                                                    D3D10_BLEND_OP_ADD, {15, 15, 15, 15, 15, 15, 15, 15}});
const D3D10_DEPTH_STENCIL_DESC kDefaultDepth = {TRUE,
                                                D3D10_DEPTH_WRITE_MASK_ALL,
                                                D3D10_COMPARISON_LESS,
                                                FALSE,
                                                0xFF,
                                                0xFF,
                                                {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS},
                                                {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS}};
const D3D10_RASTERIZER_DESC kDefaultRaster = {D3D10_FILL_SOLID, D3D10_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE};

struct Stats
{
    uint64_t draws, packets, submissions, frames, barriers, barrierCalls, psoSwitches, srvTables, samplerTables, samplerResets, splits, psoCreated, psoFailed, skippedDraws;
    double psoMs;
    uint64_t gpuTicks, gpuLists; // GPU time from the start to the end of each main list
    uint64_t depthToSrv, rtClears, dsClears;
    uint64_t scenes, dlssFrames, giPasses, motionPasses, objectMotionPasses, objectMotionCopies, bendMotionPasses, biasFrames, biasDraws, biasGiven, voxPasses, giLateReads, giOtherDepth;
    uint64_t objectMotionExactFrames, objectMotionExactDraws, objectMotionExactPasses;
    uint64_t dlssCompares; // frames the DLAA model comparison put its right half over the output
    uint64_t voxGeomDraws, voxGeomNoCamera, voxGeomSkipped, voxGeomNoPso, voxGeomPsos, voxGeomFoliage; // RunVoxelGeometry
    uint64_t voxGeomSolid; // of voxGeomDraws: not foliage
    uint64_t voxGeomStale; // replays skipped: the draw's list was done, its ring data possibly reused (VoxDraw::serial)
    uint64_t voxGeomAnimated, voxGeomFresh, voxGeomInTurn; // the cache: draws every time, new ones, level draws left to their turn
    uint64_t voxAnimatedWhy[4]; // recorded draws taken as animated: layout (bones, per-instance), a dynamic vertex buffer, no object matrix
    uint64_t voxClass[4]; // z-prepass draws recorded: discard x diffuse map (0 neither, 1 diffuse only, 2 discard only, 3 both = foliage)
    uint64_t voxPatch[5]; // solid z-prepass draws by their camera matrix: none, absolute, absolute transposed, relative, relative transposed
    uint64_t voxGeomMaterial, voxMaterialShaders;
    uint64_t foliageFrames; // BindFoliageMask: frames whose z-prepass wrote the foliage mask
    uint64_t vertices; // indices/vertices drawn (all passes)
    uint64_t zpassDraws, zpassTarget1; // draws into an R32F target with depth, of them with an SV_Target1 shader
    uint64_t voxShadowMaps, voxShadowJobs, voxShadowCovered, voxShadowRecorded, voxShadowDraws, voxShadowNoCamera; // ShadowMap
    uint64_t voxCubeSeen, voxCubeRecorded, voxCubeNoCamera, voxCubeFoliage, voxCubeRepeat, voxCubeJobs, voxCubeDraws; // cube faces
    uint64_t voxInjectRuns, voxInjectSkipped; // RunCubeInject: faces put into the levels; faces whose depth and colour differ
    uint64_t voxGeomWeapon; // z-prepass draws of the first-person view (viewport depth under 0.5) left out of the voxels
    float voxShadowCover[8]; // the last frame's shadow maps: half the width they cover around the eye (m)
    uint32_t voxShadowCoverCount;
    uint64_t voxShadowMapDraws[8], voxShadowMapSeen[8]; // per map of a frame: draws, frames
    double voxShadowMapCover[8];                        // their cover summed
    uint64_t giStampTicks[5], giStampRuns; // GI pass timers: trace, filter, upsample, voxel geometry, voxel update
    uint64_t giProbeRuns;                  // GI v2: the probes' pass runs
    uint64_t sssRuns, sssDispatches, sssNoDepth; // RunSss: frames made, their dispatches, frames without the z-prepass target
    uint64_t engineCubeDraws, engineCubeFaces[6]; // draws into the engine's environment cube (Dx12EngineEnvCube), per face
    uint64_t engineCubeUnbinds;                   // its source let go as a render target (ReleaseOutput)
    uint64_t engineCubeHdrRuns;                   // faces taken into the HDR copy (RunCubeUpdate)
    uint64_t hizRuns; // RunHiz: frames whose depth pyramid was built (the water's SSR)
    // RunVfog: frames whose fog volume was made; fog passes left as the engine's: no GI volumes this frame, the fog's
    // constants not found in its buffers, another target than the scene's (reflections, cube faces)
    uint64_t vfogRuns, vfogNoVolume, vfogNoConstants, vfogOtherTarget;
    uint64_t vfogCascadeFrames, vfogCascadeSum; // stage 2: frames with the sun's cascades in the fog, their count summed
};

// GPU busy time: a timestamp at the start and the end of every main list, resolved with the list.
constexpr int kListStamps = 256;
std::atomic<bool> g_statsResetAsked{false}; // Dx12ResetStats: m_stats and the pass times start over at the next Present

// Pipeline states are created before their first draw. Every pipeline state drawn with is recorded on disk as a recipe
// (its states, with its shaders and input layout by content), and each run starts creating all recorded ones on worker
// threads as soon as the device exists, in the order they were first needed, so draws find them ready instead of
// creating them (about a millisecond each on this driver). A draw needing one that is still queued creates it itself;
// one being created on a worker is waited for. (A D3D12 pipeline library saved nothing: loading from it cost as much as
// creating.)
const std::wstring& PipelineFile()
{
    static const std::wstring path = CryMTCacheW(L"d3d12_psos.bin");
    return path;
}

struct PsoName
{
    uint64_t h[2];
    bool operator==(const PsoName& o) const
    {
        return h[0] == o.h[0] && h[1] == o.h[1];
    }
};

struct PsoNameHash
{
    size_t operator()(const PsoName& n) const
    {
        return static_cast<size_t>(n.h[0]);
    }
};

// A graphics pipeline description without pointers: shaders and the input layout by content hash (0: none).
struct PsoRecipe
{
    uint64_t vs, gs, ps, layout;
    D3D12_BLEND_DESC blend;
    D3D12_RASTERIZER_DESC raster;
    D3D12_DEPTH_STENCIL_DESC depth;
    UINT sampleMask;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCut;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType;
    UINT numRtv;
    DXGI_FORMAT rtv[8];
    DXGI_FORMAT dsv;
    DXGI_SAMPLE_DESC samples;
};

// Field by field into a zeroed recipe, so padding bytes stay zero and names are the same in every run.
void SetBlend(D3D12_BLEND_DESC& dst, const D3D12_BLEND_DESC& src)
{
    dst.AlphaToCoverageEnable = src.AlphaToCoverageEnable;
    dst.IndependentBlendEnable = src.IndependentBlendEnable;
    for (int i = 0; i < 8; i++)
    {
        D3D12_RENDER_TARGET_BLEND_DESC& d = dst.RenderTarget[i];
        const D3D12_RENDER_TARGET_BLEND_DESC& s = src.RenderTarget[i];
        d.BlendEnable = s.BlendEnable;
        d.LogicOpEnable = s.LogicOpEnable;
        d.SrcBlend = s.SrcBlend;
        d.DestBlend = s.DestBlend;
        d.BlendOp = s.BlendOp;
        d.SrcBlendAlpha = s.SrcBlendAlpha;
        d.DestBlendAlpha = s.DestBlendAlpha;
        d.BlendOpAlpha = s.BlendOpAlpha;
        d.LogicOp = s.LogicOp;
        d.RenderTargetWriteMask = s.RenderTargetWriteMask;
    }
}

void SetDepth(D3D12_DEPTH_STENCIL_DESC& dst, const D3D12_DEPTH_STENCIL_DESC& src)
{
    dst.DepthEnable = src.DepthEnable;
    dst.DepthWriteMask = src.DepthWriteMask;
    dst.DepthFunc = src.DepthFunc;
    dst.StencilEnable = src.StencilEnable;
    dst.StencilReadMask = src.StencilReadMask;
    dst.StencilWriteMask = src.StencilWriteMask;
    dst.FrontFace = src.FrontFace;
    dst.BackFace = src.BackFace;
}

PsoName NameOf(const PsoRecipe& r)
{
    return {{HashBytes(&r, sizeof(r)), HashBytes(&r, sizeof(r), 0x9E3779B97F4A7C15ull)}};
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC DescOf(const PsoRecipe& r, const std::vector<uint8_t>* vs, const std::vector<uint8_t>* gs,
                                          const std::vector<uint8_t>* ps, const std::vector<D3D12_INPUT_ELEMENT_DESC>* layout)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = g.root;
    if (vs)
        d.VS = {vs->data(), vs->size()};
    if (gs)
        d.GS = {gs->data(), gs->size()};
    if (ps)
        d.PS = {ps->data(), ps->size()};
    d.BlendState = r.blend;
    d.SampleMask = r.sampleMask;
    d.RasterizerState = r.raster;
    d.DepthStencilState = r.depth;
    if (layout)
        d.InputLayout = {layout->data(), static_cast<UINT>(layout->size())};
    d.IBStripCutValue = r.stripCut;
    d.PrimitiveTopologyType = r.topologyType;
    d.NumRenderTargets = r.numRtv;
    for (UINT i = 0; i < r.numRtv; i++)
        d.RTVFormats[i] = r.rtv[i];
    d.DSVFormat = r.dsv;
    d.SampleDesc = r.samples;
    return d;
}

struct LayoutRecipe
{
    std::deque<std::string> names; // stable storage for the semantic names
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
};

// The file is a sequence of records {type, payload bytes, payload}, appended as new pipelines appear.
enum : uint32_t
{
    kRecordHeader = 0x534F5350, // payload: version, sizeof(PsoRecipe)
    kRecordShader = 1,          // content hash, bytecode
    kRecordLayout = 2,          // content hash, count, per element: name length, name, index, format, slot, offset, class, step
    kRecordPipeline = 3,        // PsoName, PsoRecipe
    kPipelineFileVersion = 2,
};

struct PipelineStore
{
    enum State : uint8_t
    {
        kQueued,
        kCreating,
        kReady
    };
    struct Entry
    {
        PsoRecipe recipe;
        ID3D12PipelineState* pso;
        State state;
        bool record = false; // queued at draw time: its pipeline record goes to the file once created
    };

    std::mutex lock;
    std::condition_variable readyCv;
    std::unordered_map<uint64_t, std::vector<uint8_t>> shaders;
    std::unordered_map<uint64_t, LayoutRecipe> layouts;
    std::unordered_map<PsoName, Entry, PsoNameHash> entries;
    std::deque<PsoName> queue;
    std::vector<uint8_t> unsaved; // records not yet appended to the file
    bool fileFresh = false;       // the file on disk is missing or unusable: the next save starts it over
    DWORD savedAt = 0;
    uint64_t onDisk = 0, precreated = 0, taken = 0, waited = 0, claimed = 0, created = 0, failed = 0;
    uint64_t deferred = 0, deferredDraws = 0; // pipelines created in the background at draw time, draws skipped meanwhile
    double waitMs = 0, workerMs = 0;
    int workers = 0, activeWorkers = 0;

    const std::vector<uint8_t>* Shader(uint64_t hash) const
    {
        const auto it = shaders.find(hash);
        return hash && it != shaders.end() ? &it->second : nullptr;
    }
    const std::vector<D3D12_INPUT_ELEMENT_DESC>* Layout(uint64_t hash) const
    {
        const auto it = layouts.find(hash);
        return hash && it != layouts.end() ? &it->second.elements : nullptr;
    }
};
PipelineStore g_pipes;

void AppendRecord(uint32_t type, const std::vector<uint8_t>& payload)
{
    const uint32_t head[2] = {type, static_cast<uint32_t>(payload.size())};
    const auto* p = reinterpret_cast<const uint8_t*>(head);
    g_pipes.unsaved.insert(g_pipes.unsaved.end(), p, p + sizeof(head));
    g_pipes.unsaved.insert(g_pipes.unsaved.end(), payload.begin(), payload.end());
}

template <class T>
void Put(std::vector<uint8_t>& out, const T& v)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

extern bool g_asyncPipelines;
DWORD WINAPI PipelineWorker(void*)
{
    std::unique_lock<std::mutex> lk(g_pipes.lock);
    while (!g_pipes.queue.empty())
    {
        const PsoName name = g_pipes.queue.front();
        g_pipes.queue.pop_front();
        PipelineStore::Entry& e = g_pipes.entries[name];
        if (e.state != PipelineStore::kQueued)
            continue;
        e.state = PipelineStore::kCreating;
        // The store's shaders and layouts are never removed and map nodes do not move, so the description stays valid.
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC d =
            DescOf(e.recipe, g_pipes.Shader(e.recipe.vs), g_pipes.Shader(e.recipe.gs), g_pipes.Shader(e.recipe.ps), g_pipes.Layout(e.recipe.layout));
        lk.unlock();
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceCounter(&t0);
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(g.dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso))))
            pso = nullptr;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        lk.lock();
        g_pipes.workerMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        e.pso = pso;
        e.state = PipelineStore::kReady;
        g_pipes.precreated += pso != nullptr;
        if (e.record && pso)
        {
            std::vector<uint8_t> payload;
            Put(payload, name);
            Put(payload, e.recipe);
            AppendRecord(kRecordPipeline, payload);
        }
        g_pipes.readyCv.notify_all();
    }
    g_pipes.activeWorkers--;
    return 0;
}

// Reads the recipes on disk and starts creating their pipelines.
void OpenPipelines()
{
    {
        wchar_t value[8] = {};
        g_asyncPipelines = !(GetEnvironmentVariableW(L"CRYMT_ASYNCPSO", value, 8) && wcscmp(value, L"0") == 0);
    }
    std::vector<uint8_t> data;
    HANDLE file = CreateFileW(PipelineFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size{};
        GetFileSizeEx(file, &size);
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size())
            data.clear();
        CloseHandle(file);
    }
    std::lock_guard<std::mutex> guard(g_pipes.lock);
    size_t at = 0;
    bool valid = false;
    while (at + 8 <= data.size())
    {
        uint32_t head[2];
        memcpy(head, &data[at], 8);
        if (at + 8 + head[1] > data.size())
            break; // a record cut short by an interrupted save
        const uint8_t* p = &data[at + 8];
        const uint32_t bytes = head[1];
        at += 8 + bytes;
        if (!valid)
        {
            const uint32_t expect[2] = {kPipelineFileVersion, sizeof(PsoRecipe)};
            valid = head[0] == kRecordHeader && bytes == sizeof(expect) && memcmp(p, expect, sizeof(expect)) == 0;
            if (!valid)
                break;
            continue;
        }
        if (head[0] == kRecordShader && bytes >= 8)
        {
            uint64_t hash;
            memcpy(&hash, p, 8);
            g_pipes.shaders.try_emplace(hash, p + 8, p + bytes);
        }
        else if (head[0] == kRecordLayout && bytes >= 12)
        {
            uint64_t hash;
            uint32_t count;
            memcpy(&hash, p, 8);
            memcpy(&count, p + 8, 4);
            LayoutRecipe& l = g_pipes.layouts[hash];
            l.names.clear();
            l.elements.clear();
            size_t q = 12;
            for (uint32_t i = 0; i < count && q + 4 <= bytes; i++)
            {
                uint32_t len;
                memcpy(&len, p + q, 4);
                q += 4;
                if (q + len + 24 > bytes)
                    break;
                l.names.emplace_back(reinterpret_cast<const char*>(p + q), len);
                q += len;
                uint32_t f[6];
                memcpy(f, p + q, sizeof(f));
                q += sizeof(f);
                l.elements.push_back({l.names.back().c_str(), f[0], static_cast<DXGI_FORMAT>(f[1]), f[2], f[3], static_cast<D3D12_INPUT_CLASSIFICATION>(f[4]), f[5]});
            }
        }
        else if (head[0] == kRecordPipeline && bytes == sizeof(PsoName) + sizeof(PsoRecipe))
        {
            PsoName name;
            PsoRecipe r;
            memcpy(&name, p, sizeof(name));
            memcpy(&r, p + sizeof(name), sizeof(r));
            if (!g_pipes.Shader(r.vs) || (r.gs && !g_pipes.Shader(r.gs)) || (r.ps && !g_pipes.Shader(r.ps)) || (r.layout && !g_pipes.Layout(r.layout)))
                continue;
            if (g_pipes.entries.try_emplace(name, PipelineStore::Entry{r, nullptr, PipelineStore::kQueued}).second)
                g_pipes.queue.push_back(name);
        }
    }
    g_pipes.onDisk = g_pipes.queue.size();
    if (valid && at != data.size())
    {
        // A save was interrupted: rewrite the good part so new records do not land behind the broken one.
        g_pipes.fileFresh = true;
        g_pipes.unsaved.assign(data.begin(), data.begin() + at);
    }
    if (!valid)
    {
        // Missing or from another version: start the file over with the next save.
        g_pipes.shaders.clear();
        g_pipes.layouts.clear();
        g_pipes.entries.clear();
        g_pipes.queue.clear();
        g_pipes.onDisk = 0;
        g_pipes.fileFresh = true;
        const uint32_t header[2] = {kPipelineFileVersion, sizeof(PsoRecipe)};
        AppendRecord(kRecordHeader, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(header), reinterpret_cast<const uint8_t*>(header) + sizeof(header)));
    }
    // Workers below normal priority, leaving the game's own threads a core or two.
    const DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    g_pipes.workers = g_pipes.queue.empty() ? 0 : static_cast<int>(std::max<DWORD>(1, std::min<DWORD>(6, cpus / 2 > 1 ? cpus / 2 - 1 : 1)));
    for (int i = 0; i < g_pipes.workers; i++)
    {
        g_pipes.activeWorkers++;
        if (HANDLE t = CreateThread(nullptr, 0, PipelineWorker, nullptr, 0, nullptr))
        {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(t);
        }
        else
            g_pipes.activeWorkers--;
    }
    g_pipes.savedAt = GetTickCount();
    Log("pipelines: %llu recipes on disk (%zu bytes, %zu shaders, %zu layouts), %d workers", static_cast<unsigned long long>(g_pipes.onDisk), data.size(),
        g_pipes.shaders.size(), g_pipes.layouts.size(), g_pipes.workers);
}

// Context thread: the pipeline with this name if it was created ahead (waiting for it if a worker is on it); null when
// the caller has to create it and hand it to PublishPipeline.
ID3D12PipelineState* TakePipeline(const PsoName& name)
{
    std::unique_lock<std::mutex> lk(g_pipes.lock);
    const auto it = g_pipes.entries.find(name);
    if (it == g_pipes.entries.end())
        return nullptr;
    PipelineStore::Entry& e = it->second;
    if (e.state == PipelineStore::kQueued)
    {
        e.state = PipelineStore::kCreating; // the workers skip it
        g_pipes.claimed++;
        return nullptr;
    }
    if (e.state == PipelineStore::kCreating)
    {
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceCounter(&t0);
        g_pipes.readyCv.wait(lk, [&e] { return e.state == PipelineStore::kReady; });
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        g_pipes.waitMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        g_pipes.waited++;
    }
    if (!e.pso)
    {
        e.state = PipelineStore::kCreating; // failed on a worker: the caller tries again
        return nullptr;
    }
    g_pipes.taken++;
    return e.pso;
}

// With the store locked: the shaders and the layout of a recipe the file lacks, into the store and the file.
void RecordShadersAndLayout(const PsoRecipe& r, const std::vector<uint8_t>* const code[3], const std::vector<D3D12_INPUT_ELEMENT_DESC>* layout)
{
    const uint64_t hashes[3] = {r.vs, r.gs, r.ps};
    for (int i = 0; i < 3; i++)
        if (hashes[i] && code[i] && g_pipes.shaders.try_emplace(hashes[i], *code[i]).second)
        {
            std::vector<uint8_t> payload;
            Put(payload, hashes[i]);
            payload.insert(payload.end(), code[i]->begin(), code[i]->end());
            AppendRecord(kRecordShader, payload);
        }
    if (r.layout && layout && !g_pipes.layouts.count(r.layout))
    {
        LayoutRecipe& l = g_pipes.layouts[r.layout];
        std::vector<uint8_t> payload;
        Put(payload, r.layout);
        Put(payload, static_cast<uint32_t>(layout->size()));
        for (const D3D12_INPUT_ELEMENT_DESC& e : *layout)
        {
            l.names.emplace_back(e.SemanticName ? e.SemanticName : "");
            l.elements.push_back(e);
            l.elements.back().SemanticName = l.names.back().c_str();
            Put(payload, static_cast<uint32_t>(l.names.back().size()));
            payload.insert(payload.end(), l.names.back().begin(), l.names.back().end());
            const uint32_t f[6] = {e.SemanticIndex, static_cast<uint32_t>(e.Format), e.InputSlot, e.AlignedByteOffset, static_cast<uint32_t>(e.InputSlotClass),
                                   e.InstanceDataStepRate};
            payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(f), reinterpret_cast<const uint8_t*>(f) + sizeof(f));
        }
        AppendRecord(kRecordLayout, payload);
    }
}

// Context thread: a pipeline the caller created; new ones are recorded with whatever shaders and layout the file lacks.
void PublishPipeline(const PsoName& name, const PsoRecipe& r, ID3D12PipelineState* pso, const std::vector<uint8_t>* const code[3],
                     const std::vector<D3D12_INPUT_ELEMENT_DESC>* layout)
{
    std::lock_guard<std::mutex> guard(g_pipes.lock);
    const auto [it, fresh] = g_pipes.entries.try_emplace(name, PipelineStore::Entry{r, pso, PipelineStore::kReady});
    it->second.pso = pso;
    it->second.state = PipelineStore::kReady;
    g_pipes.readyCv.notify_all();
    if (pso)
        g_pipes.created++;
    else
        g_pipes.failed++;
    if (!fresh || !pso)
        return;
    RecordShadersAndLayout(r, code, layout);
    std::vector<uint8_t> payload;
    Put(payload, name);
    Put(payload, r);
    AppendRecord(kRecordPipeline, payload);
}

// Pipelines missing at draw time (CRYMT_ASYNCPSO, default on): a draw that may be skipped (an indexed draw with depth
// testing: scene objects, shadow casters, depth-tested transparents) does not wait for the driver (one new pipeline cost
// 81 ms on harbor). Its pipeline is created on a worker and the draw is skipped until it is ready, as modern engines do
// (UE5's PSO precaching skips draws; Crysis' own r_ShadersAsyncCompiling skipped objects while their shader compiled).
// Full-screen passes and the HUD (no depth test) still create theirs at once. The new recipe goes to the file as usual.
bool g_asyncPipelines = true;
std::atomic<uint64_t> g_presented{0}; // frames presented (CRYMT_PSOLOG: when a pipeline was missing)

void EnsurePipelineWorker()
{
    if (g_pipes.activeWorkers >= 2)
        return;
    g_pipes.activeWorkers++;
    if (HANDLE t = CreateThread(nullptr, 0, PipelineWorker, nullptr, 0, nullptr))
        CloseHandle(t);
    else
        g_pipes.activeWorkers--;
}

// Context thread: the pipeline if it is ready (pending false); else it is queued at the front and pending is true.
ID3D12PipelineState* TakePipelineAsync(const PsoName& name, const PsoRecipe& r, const std::vector<uint8_t>* const code[3],
                                       const std::vector<D3D12_INPUT_ELEMENT_DESC>* layout, bool& pending)
{
    std::lock_guard<std::mutex> guard(g_pipes.lock);
    pending = true;
    auto it = g_pipes.entries.find(name);
    if (it == g_pipes.entries.end())
    {
        RecordShadersAndLayout(r, code, layout);
        PipelineStore::Entry e{r, nullptr, PipelineStore::kQueued};
        e.record = true;
        g_pipes.entries.emplace(name, e);
        g_pipes.queue.push_front(name);
        g_pipes.deferred++;
        EnsurePipelineWorker();
        g_pipes.deferredDraws++;
        static const bool psoLog = GetEnvironmentVariableW(L"CRYMT_PSOLOG", nullptr, 0) != 0;
        if (psoLog && g_pipes.deferred <= 400) // diagnostics: which draws wait for a new pipeline, and when
            Log("pipeline missing at frame %llu: vs %016llx ps %016llx rtv0 %d dsv %d depth %d/%d blend %d cull %d\n",
                static_cast<unsigned long long>(g_presented.load()), static_cast<unsigned long long>(r.vs), static_cast<unsigned long long>(r.ps),
                static_cast<int>(r.rtv[0]), static_cast<int>(r.dsv), r.depth.DepthEnable, static_cast<int>(r.depth.DepthFunc),
                r.blend.RenderTarget[0].BlendEnable, static_cast<int>(r.raster.CullMode));
        return nullptr;
    }
    PipelineStore::Entry& e = it->second;
    if (e.state == PipelineStore::kQueued)
    {
        g_pipes.queue.push_front(name); // the stale queue position is skipped by the worker
        EnsurePipelineWorker();
    }
    if (e.state != PipelineStore::kReady)
    {
        g_pipes.deferredDraws++;
        return nullptr;
    }
    pending = false;
    g_pipes.taken++;
    return e.pso;
}

// Context thread: appends the new records to the file.
void SavePipelines()
{
    std::vector<uint8_t> data;
    bool fresh;
    {
        std::lock_guard<std::mutex> guard(g_pipes.lock);
        data.swap(g_pipes.unsaved);
        fresh = g_pipes.fileFresh;
        g_pipes.fileFresh = false;
    }
    if (data.empty())
        return;
    HANDLE file = CreateFileW(PipelineFile().c_str(), fresh ? GENERIC_WRITE : FILE_APPEND_DATA, 0, nullptr, fresh ? CREATE_ALWAYS : OPEN_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
}

template <class T>
void Bind(T*& slot, T* value)
{
    if (slot == value)
        return;
    if (value)
        value->AddRef();
    if (slot)
        slot->Release();
    slot = value;
}

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

    // A 4x4 texture of the CRYMT_ENVTEST colour (a cube: 6 faces).
    Srv12* EnvTestTexture(bool cube)
    {
        const uint32_t c = EnvTestColour();
        uint32_t texels[4 * 4 * 6];
        for (uint32_t& x : texels)
            x = 0xFF000000u | (c >> 16 & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16; // R8G8B8A8: R in the low byte
        D3D10_TEXTURE2D_DESC d{4, 4, 1, cube ? 6u : 1u, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE, 0,
                               cube ? static_cast<UINT>(D3D10_RESOURCE_MISC_TEXTURECUBE) : 0u};
        D3D10_SUBRESOURCE_DATA init[6];
        for (int f = 0; f < 6; f++)
            init[f] = {texels + f * 16, 16, 64};
        ID3D10Texture2D* tex = nullptr;
        ID3D10ShaderResourceView* view = nullptr;
        Srv12* out = nullptr;
        if (SUCCEEDED(CreateTexture2D(&d, init, &tex)) && SUCCEEDED(CreateShaderResourceView(tex, nullptr, &view)))
            out = static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(view));
        if (tex)
            tex->Release(); // the view keeps it
        return out;
    }

    // CRYMT_ENVMAP=voxels: the materials' environment maps from the voxel grid (with the GI's voxel part on).
    // ... or the panel's Num0 on its environment page (CRYMT_GI_KEYS).
    // The materials' reflection: 0 the game's maps (CRYMT_ENVMAP=game), 1 the voxel grid's cube (=voxels), 2 the engine's own
    // cube (=engine, the default since 2026-11-13: its HDR copy encoded as the voxel one, with mips by gloss); the page.
    static int EnvModeBase()
    {
        static const int mode = [] {
            char v[16] = {};
            if (!GetEnvironmentVariableA("CRYMT_ENVMAP", v, sizeof(v)))
                return 2;
            return _stricmp(v, "voxels") == 0 ? 1 : _stricmp(v, "engine") == 0 ? 2 : 0;
        }();
        return mode;
    }
    static int EnvMode()
    {
        const int key = g_giControl.env;
        return g_giControl.active && key >= 0 ? std::min(key, 2) : EnvModeBase();
    }
    static bool EnvOn() { return EnvMode() == 1; }       // the voxel cube (made by the GI pass)
    static bool EnvEngineOn() { return EnvMode() == 2; } // the engine's cube

    // The ocean's reflection (reflections phases 0 and 1): 0 planar, 1 the environment cube, 2 SSR + the cube;
    // CRYMT_WATERCUBE=1/2 or the reflections page.
    // Default 2 since 2026-11-13 (the user: "turn our new reflections on by default").
    static int WaterModeBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_WATERCUBE", v, 8) ? std::min(std::max(_wtoi(v), 0), 2) : 2;
        }();
        return env;
    }
    static int WaterMode()
    {
        const int panel = g_giControl.waterCube;
        return panel < 0 ? WaterModeBase() : std::min(panel, 2);
    }
    static bool WaterCubeOn() { return WaterMode() >= 1; }
    // The cube's brightness on the water (CRYMT_WATERSKY, the reflections page, crymt_watersky), 1 = as decoded. Default
    // 0.75: the low sea view against the planar reflection (2026-11-13, wsky): water luma +13% at 1.0, -2% at 0.7; the
    // cube's sky near the horizon is greyer (blue -5..-7) at any scale.
    static float WaterSkyBase()
    {
        static const float env = [] {
            wchar_t v[16] = {};
            return GetEnvironmentVariableW(L"CRYMT_WATERSKY", v, 16) ? static_cast<float>(_wtof(v)) : 0.75f;
        }();
        return env > 0 ? env : 0.75f;
    }
    static float WaterSky()
    {
        const float panel = g_giControl.waterSky;
        return panel > 0 ? panel : WaterSkyBase();
    }
    static bool WaterSsrOn() { return WaterMode() == 2; }
    // The water's cube (reflections phase 4): 1 the engine's own capture at the camera (CryMT.cpp EnvCubeUpdate: one face
    // a frame, RGBK), 0 the voxel grid's; CRYMT_ENVCUBE or the reflections page. Default the engine's.
    static bool EngineCubeOn()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE", v, 8) ? _wtoi(v) : 1;
        }();
        const int panel = g_giControl.waterSource;
        return (panel < 0 ? env : panel) == 1;
    }
    // How often the engine's cube draws a face (CryMT.cpp EnvCubeUpdate reads the panel's value): every frame (the whole
    // cube in 6 frames) to every 8th (48 frames); CRYMT_ENVCUBE_RATE, the reflections page.
    static constexpr int kEnvCubeRates[6] = {1, 2, 3, 4, 6, 8};
    static int EnvCubeRateBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_RATE", v, 8) ? std::min(std::max(_wtoi(v), 1), 8) : 1;
        }();
        return env;
    }
    static int EnvCubeRateIndex(int rate)
    {
        int i = 0;
        while (i < 5 && kEnvCubeRates[i + 1] <= rate)
            i++;
        return i;
    }
    // The culling of the engine's cube faces (in its render: m_engineCubePass): 0 as the engine sets it, 1 none, 2 the
    // other side, the default (2026-11-13: the faces lacked the near terrain and the houses, palms stayed: the face
    // cameras' images are mirrored, so the winding turns and the front faces were the ones culled; a hint the user was
    // given: "r->SetCullMode(R_CULL_NONE)"). CRYMT_ENVCUBE_CULL, the reflections page.
    static int EngineCubeCullBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_CULL", v, 8) ? std::min(std::max(_wtoi(v), 0), 3) : 3;
        }();
        return env;
    }
    static int EngineCubeCull()
    {
        const int panel = g_giControl.envCubeCull;
        return panel < 0 ? EngineCubeCullBase() : std::min(panel, 3);
    }
    // 3 (the default since 2026-11-13 night): per face of the cube (its D3D slice, +X -X +Y -Y +Z -Z). Village, each
    // slice drawn with every mode and compared with no culling (closed meshes look the same with the right one):
    // slices 0 1 2 4 5 the inverted one, slice 3 (-Y) the engine's (its image is not mirrored; the walls had holes in
    // it - the user). Island 2026-11-14 (tools\prof\cube_modes.ps1 + cube_faces_vs.py, against no culling): slice 3
    // inverted 22% of pixels off, the engine's 6%; the others inverted 1-3%, the engine's 4-10%. The table was indexed
    // by the face number of the start mark (the engine's entry +8), which is not the slice the render ends in: slice 3
    // got the inverted culling (the user: the defaults showed the cube wrong). Now by the slice that number was last
    // seen to end in (g_cubeSliceOfMark, learned at each render's end; until then the number itself).
    static int EngineCubeSlice(int markFace)
    {
        return markFace >= 0 && markFace < 6 && g_cubeSliceOfMark[markFace] >= 0 ? g_cubeSliceOfMark[markFace] : markFace;
    }
    static int EngineCubeCullFor(int markFace)
    {
        static constexpr int kPerSlice[6] = {2, 2, 2, 0, 2, 2};
        const int mode = EngineCubeCull();
        const int slice = EngineCubeSlice(markFace);
        return mode < 3 ? mode : slice >= 0 && slice < 6 ? kPerSlice[slice] : 2;
    }
    // A new face mixed into the HDR copy (CRYMT_ENVCUBE_BLEND, the reflections page): 1 replaces, 0.5 half (default), 0.25.
    static float EngineCubeBlend()
    {
        static const float env = [] {
            wchar_t v[16] = {};
            const float b = GetEnvironmentVariableW(L"CRYMT_ENVCUBE_BLEND", v, 16) ? static_cast<float>(_wtof(v)) : 0.5f;
            return b > 0.0f && b <= 1.0f ? b : 0.5f;
        }();
        const int panel = g_giControl.envCubeBlend;
        return panel < 0 ? env : panel == 0 ? 1.0f : panel == 1 ? 0.5f : 0.25f;
    }
    static int EngineCubeBlendIndex(float b) { return b >= 0.99f ? 0 : b >= 0.4f ? 1 : 2; }
    static float EngineCubeBlendBase()
    {
        const int saved = g_giControl.envCubeBlend.exchange(-1);
        const float b = EngineCubeBlend();
        g_giControl.envCubeBlend = saved;
        return b;
    }
    // The -Z face (the water's reflections look up): CRYMT_ENVCUBE_DOWN 1 draws it, default 0 (once after each start).
    static int EngineCubeDownBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_DOWN", v, 8) ? (_wtoi(v) != 0 ? 1 : 0) : 0;
        }();
        return env;
    }
    // Entities in the engine's cube (clouds, vehicles, characters): CRYMT_ENVCUBE_ENTITIES, default 1 (CryMT.cpp draws them).
    static int EngineCubeEntitiesBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_ENTITIES", v, 8) ? (_wtoi(v) != 0 ? 1 : 0) : 1;
        }();
        return env;
    }
    // The clouds in it: CRYMT_ENVCUBE_CLOUDS, default 0 (CryMT.cpp leaves them out).
    static int EngineCubeCloudsBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_CLOUDS", v, 8) ? (_wtoi(v) != 0 ? 1 : 0) : 0;
        }();
        return env;
    }
    // Its brightness on the water (CRYMT_ENVCUBE_SCALE, default 1: the engine drew it as it draws the planar reflection).
    static float EngineCubeScale()
    {
        static const float env = [] {
            wchar_t v[16] = {};
            const float s = GetEnvironmentVariableW(L"CRYMT_ENVCUBE_SCALE", v, 16) ? static_cast<float>(_wtof(v)) : 1.0f;
            return s > 0 ? s : 1.0f;
        }();
        return env;
    }

    // The engine's cube as the water reads it (stream thread): a cube view of its first level (the engine draws only that)
    // made when the render thread hands over another texture; null until it has all six faces.
    Srv12* EngineCube()
    {
        ID3D10Resource* res = nullptr;
        {
            std::lock_guard<std::mutex> guard(g_engineCubeLock);
            res = g_engineCubePending;
        }
        if (res == m_engineCubeRes)
            return m_engineCube;
        if (m_engineCube)
            m_engineCube->Release();
        m_engineCube = nullptr;
        m_engineCubeRes = res;
        ID3D10Texture2D* tex = nullptr; // Dx12EngineEnvCube is given a 2D texture; asked anyway
        if (!res || FAILED(res->QueryInterface(__uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&tex))))
            return nullptr;
        D3D10_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        tex->Release();
        D3D10_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                 : td.Format;
        sd.ViewDimension = D3D10_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MostDetailedMip = 0;
        sd.TextureCube.MipLevels = 1;
        ID3D10ShaderResourceView* view = nullptr;
        if (td.ArraySize != 6 || !(td.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE) || FAILED(CreateShaderResourceView(res, &sd, &view)))
        {
            Log("engine environment cube: no view (%ux%u format %d, array %u, misc %x)", td.Width, td.Height, static_cast<int>(td.Format),
                td.ArraySize, td.MiscFlags);
            return nullptr;
        }
        m_engineCube = static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(view));
        m_engineCubeViews++;
        Log("engine environment cube for the water: %ux%u format %d, %u levels (the first read)", td.Width, td.Height,
            static_cast<int>(td.Format), td.MipLevels);
        return m_engineCube;
    }
    // The cube the water reads (Water.cfx's waterenv.dds placeholder): kind 0 the voxel grid's, 1 the engine's RGBK cube as
    // it is (the faces flipped: the shader turns its lookup), 2 the HDR copy with mips (D3D's layout), once it has all faces.
    struct EnvView
    {
        ResourceInfo* info = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        int kind = 0;
    };
    EnvView WaterEnvCube()
    {
        EnvView v;
        if (!WaterCubeOn())
            return v;
        if (EngineCubeOn())
        {
            Srv12* raw = EngineCube();
            bool all = m_hdrCube.res != nullptr;
            for (bool f : m_hdrFaceValid)
                all = all && f;
            if (raw && all)
            {
                v.info = &m_hdrCube;
                v.cpu = m_hdrCubeSrv;
                v.kind = 2;
            }
            else if (raw)
            {
                v.info = raw->info;
                v.cpu = raw->cpu;
                v.kind = 1;
            }
            return v;
        }
        if (m_envReady && m_envCubeAll)
        {
            v.info = m_envCubeAll->info;
            v.cpu = m_envCubeAll->cpu;
        }
        return v;
    }

    bool EnsureEnvTargets()
    {
        if (m_envAtlas[0] || m_envFailed)
            return m_envAtlas[0] != nullptr;
        m_envFailed = true;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = kEnvSize * 26;
        d.Height = kEnvSize;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (int k = 0; k < 2; k++)
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                      k ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_envAtlas[k]))))
            {
                Log("environment maps: no atlas");
                if (m_envAtlas[0])
                    m_envAtlas[0]->Release();
                m_envAtlas[0] = nullptr;
                return false;
            }
        for (int k = 0; k < 3; k++)
        {
            D3D10_TEXTURE2D_DESC td{kEnvSize, kEnvSize, 1, k == 1 ? 1u : 6u, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_SHADER_RESOURCE, 0,
                                    k == 1 ? 0u : static_cast<UINT>(D3D10_RESOURCE_MISC_TEXTURECUBE)};
            ID3D10Texture2D* tex = nullptr;
            ID3D10ShaderResourceView* view = nullptr;
            if (SUCCEEDED(CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(CreateShaderResourceView(tex, nullptr, &view)))
                (k == 2 ? m_envCubeAll : k ? m_env2D : m_envCube) = static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(view));
            if (tex)
                tex->Release(); // the view keeps it
        }
        if (!m_envCube || !m_env2D || !m_envCubeAll)
        {
            m_envAtlas[0]->Release();
            m_envAtlas[1]->Release();
            m_envAtlas[0] = m_envAtlas[1] = nullptr;
            Log("environment maps: no cube or 2D map");
            return false;
        }
        m_envFailed = false;
        Log("environment maps from the voxel grid: %ux%u cube and 2D map, every 4th frame", kEnvSize, kEnvSize);
        return true;
    }

    // What the material's environment map in this slot of the pixel shader is replaced by (null: kept): the CRYMT_ENVTEST
    // colour (stage 0), or the maps from the voxel grid once they have been made.
    // A material's own environment map replaced: the voxel cube / 2D map (mode 1), the engine's cube / the 2D map made from
    // it (mode 2; until 2026-11-15 the 2D maps kept the game's static one in this mode: the first-person weapons' Metal).
    // Every material alike (by its samplers' names), but water and glass (IsRefractiveResource) and eyes, hair, skin.
    EnvView EnvSubstitute(int slot)
    {
        EnvView v;
        if (!m_ps || (slot != m_ps->envCubeSlot && slot != m_ps->env2DSlot) || m_ps->refractive || m_ps->noEnv)
            return v; // water, glass: their maps stay (the user: the river came out overbright); eyes, hair, skin too
        if (EnvTestColour() == 0xFFFFFFFFu)
        {
            if (EnvEngineOn() && m_ldrCubeReady && slot == m_ps->envCubeSlot)
            {
                v.info = &m_ldrCubeOwn;
                v.cpu = m_ldrCubeOwnSrv;
            }
            else if (EnvEngineOn() && m_ldr2DReady && slot == m_ps->env2DSlot)
            {
                v.info = &m_ldr2D;
                v.cpu = m_ldr2DSrv;
            }
            else if (Srv12* s = EnvOn() && m_envReady ? (slot == m_ps->envCubeSlot ? m_envCube : m_env2D) : nullptr)
            {
                v.info = s->info;
                v.cpu = s->cpu;
            }
            return v;
        }
        if (!m_envTestTried)
        {
            m_envTestTried = true;
            m_envTestCube = EnvTestTexture(true);
            m_envTest2D = EnvTestTexture(false);
            Log("environment test textures %06x: cube %s, 2D %s", EnvTestColour(), m_envTestCube ? "ready" : "failed", m_envTest2D ? "ready" : "failed");
        }
        if (Srv12* s = slot == m_ps->envCubeSlot ? m_envTestCube : m_envTest2D)
        {
            v.info = s->info;
            v.cpu = s->cpu;
        }
        return v;
    }

    // In the engine's cube render (m_engineCubePass): its encode pass binds the face's scene (TempEnvCMap) as its input
    // while it is still the render target (the cube's face is set after, applied at the draw), which D3D10 turns into
    // no input (the cube came out black). There the target is let go instead: the engine replaces it before the draw.
    bool ReleaseOutput(Srv12* v)
    {
        for (Rtv12*& rtv : m_rtv)
            if (rtv && rtv->resource == v->resource)
            {
                rtv->info->outputBound--;
                Bind(rtv, static_cast<Rtv12*>(nullptr));
                m_omDirty = m_psoDirty = true;
                m_stats.engineCubeUnbinds++;
            }
        return v->info->outputBound == 0;
    }

    void SetShaderResources(Stage s, UINT start, UINT n, ID3D10ShaderResourceView* const* views)
    {
        StageState& st = m_stage[s];
        for (UINT i = 0; i < n && start + i < D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++)
        {
            auto* v = views ? static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(views[i])) : nullptr;
            if (v && v->info->outputBound && !(m_engineCubePass && ReleaseOutput(v))) // D3D10: a resource bound for output cannot be an input
                v = nullptr;

            if (st.srv[start + i] != v)
            {
                Bind(st.srv[start + i], v);
                st.srvDirty = true;
            }
        }
        if (n)
            st.srvEnd = std::max(st.srvEnd, std::min(start + n, static_cast<UINT>(D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)));
    }

    void SetSamplers(Stage s, UINT start, UINT n, ID3D10SamplerState* const* samplers)
    {
        StageState& st = m_stage[s];
        for (UINT i = 0; i < n && start + i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++)
        {
            auto* smp = samplers ? static_cast<Sampler12*>(samplers[i]) : nullptr;
            if (st.sampler[start + i] != smp)
            {
                Bind(st.sampler[start + i], smp);
                st.samplerDirty = true;
            }
        }
    }

    template <class T>
    void GetArray(T* const* from, UINT start, UINT n, T** to, UINT limit)
    {
        for (UINT i = 0; i < n; i++)
        {
            T* v = start + i < limit ? from[start + i] : nullptr;
            if (v)
                v->AddRef();
            to[i] = v;
        }
    }

    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT start, UINT n, ID3D10Buffer* const* b) override
    {
        SetConstantBuffers(kVS, start, n, b);
    }
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT start, UINT n, ID3D10Buffer* const* b) override
    {
        SetConstantBuffers(kGS, start, n, b);
    }
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT start, UINT n, ID3D10Buffer* const* b) override
    {
        SetConstantBuffers(kPS, start, n, b);
    }
    void STDMETHODCALLTYPE VSSetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView* const* v) override
    {
        SetShaderResources(kVS, start, n, v);
    }
    void STDMETHODCALLTYPE GSSetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView* const* v) override
    {
        SetShaderResources(kGS, start, n, v);
    }
    void STDMETHODCALLTYPE PSSetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView* const* v) override
    {
        SetShaderResources(kPS, start, n, v);
    }
    void STDMETHODCALLTYPE VSSetSamplers(UINT start, UINT n, ID3D10SamplerState* const* s) override
    {
        SetSamplers(kVS, start, n, s);
    }
    void STDMETHODCALLTYPE GSSetSamplers(UINT start, UINT n, ID3D10SamplerState* const* s) override
    {
        SetSamplers(kGS, start, n, s);
    }
    void STDMETHODCALLTYPE PSSetSamplers(UINT start, UINT n, ID3D10SamplerState* const* s) override
    {
        SetSamplers(kPS, start, n, s);
    }

    void STDMETHODCALLTYPE VSSetShader(ID3D10VertexShader* s) override
    {
        auto* v = static_cast<VS12*>(s);
        if (v != m_vs)
        {
            Bind(m_vs, v);
            m_psoDirty = true;
        }
    }
    void STDMETHODCALLTYPE GSSetShader(ID3D10GeometryShader* s) override
    {
        auto* v = static_cast<GS12*>(s);
        if (v != m_gs)
        {
            Bind(m_gs, v);
            m_psoDirty = true;
        }
    }
    void STDMETHODCALLTYPE PSSetShader(ID3D10PixelShader* s) override
    {
        auto* v = static_cast<PS12*>(s);
        if (v != m_ps)
        {
            const auto env = [](const PS12* p) { return p && (p->envCubeSlot >= 0 || p->env2DSlot >= 0); };
            if (env(v) || env(m_ps))
                m_stage[kPS].srvDirty = true;
            Bind(m_ps, v);
            m_psoDirty = true;
        }
    }

    // ---- input assembler
    void STDMETHODCALLTYPE IASetInputLayout(ID3D10InputLayout* layout) override
    {
        auto* l = static_cast<Layout12*>(layout);
        if (l != m_layout)
        {
            Bind(m_layout, l);
            m_psoDirty = true;
        }
    }

    void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT n, ID3D10Buffer* const* buffers, const UINT* strides, const UINT* offsets) override
    {
        for (UINT i = 0; i < n && start + i < 16; i++)
        {
            VertexBinding& vb = m_vb[start + i];
            Bind(vb.buffer, buffers ? static_cast<Buffer12*>(buffers[i]) : nullptr);
            vb.stride = strides ? strides[i] : 0;
            vb.offset = offsets ? offsets[i] : 0;
        }
    }

    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D10Buffer* buffer, DXGI_FORMAT format, UINT offset) override
    {
        Bind(m_ib, static_cast<Buffer12*>(buffer));
        if (format != m_ibFormat && StripTopology(m_topology))
            m_psoDirty = true;
        m_ibFormat = format;
        m_ibOffset = offset;
    }

    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY t) override
    {
        if (t == m_topology)
            return;
        if (TopologyType(t) != TopologyType(m_topology) || StripTopology(t) != StripTopology(m_topology))
            m_psoDirty = true;
        m_topology = t;
        m_topologyDirty = true;
    }

    // ---- output merger
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, ID3D10RenderTargetView* const* views, ID3D10DepthStencilView* depth) override
    {
        Rtv12* rtv[8] = {};
        for (UINT i = 0; i < n && i < 8 && views; i++)
            rtv[i] = static_cast<Rtv12*>(views[i]);
        auto* dsv = static_cast<Dsv12*>(depth);
        for (UINT i = 0; i < 8; i++)
        {
            if (rtv[i])
                rtv[i]->info->outputBound++;
            if (m_rtv[i])
                m_rtv[i]->info->outputBound--;
        }
        if (dsv)
            dsv->info->outputBound++;
        if (m_dsv)
            m_dsv->info->outputBound--;
        for (UINT i = 0; i < 8; i++)
        {
            Bind(m_rtv[i], rtv[i]);
            if (rtv[i])
                UnbindInputs(rtv[i]->resource);
        }
        Bind(m_dsv, dsv);
        if (dsv)
            UnbindInputs(dsv->resource);
        m_omDirty = true;
        m_psoDirty = true;
    }

    // D3D10's hazard rule: a resource bound for output is unbound from every shader input slot.
    void UnbindInputs(ID3D10Resource* resource)
    {
        for (StageState& st : m_stage)
            for (UINT i = 0; i < st.srvEnd; i++)
                if (st.srv[i] && st.srv[i]->resource == resource)
                {
                    Bind(st.srv[i], static_cast<Srv12*>(nullptr));
                    st.srvDirty = true;
                }
    }

    void STDMETHODCALLTYPE OMSetBlendState(ID3D10BlendState* state, const FLOAT factor[4], UINT mask) override
    {
        auto* b = static_cast<Blend12*>(static_cast<ID3D10BlendState1*>(state));
        if (b != m_blend || mask != m_sampleMask)
            m_psoDirty = true;
        Bind(m_blend, b);
        m_sampleMask = mask;
        const float one[4] = {1, 1, 1, 1};
        const float* f = factor ? factor : one;
        if (memcmp(f, m_blendFactor, sizeof(m_blendFactor)) != 0)
        {
            memcpy(m_blendFactor, f, sizeof(m_blendFactor));
            m_blendFactorDirty = true;
        }
    }

    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D10DepthStencilState* state, UINT ref) override
    {
        auto* d = static_cast<Depth12*>(state);
        if (d != m_depth)
        {
            Bind(m_depth, d);
            m_psoDirty = true;
        }
        if (ref != m_stencilRef)
        {
            m_stencilRef = ref;
            m_stencilRefDirty = true;
        }
    }

    void STDMETHODCALLTYPE SOSetTargets(UINT n, ID3D10Buffer* const* buffers, const UINT*) override
    {
        for (UINT i = 0; i < n; i++)
            if (buffers && buffers[i])
                LogLimited("so", "SOSetTargets with a buffer is not supported");
    }

    void STDMETHODCALLTYPE SetPredication(ID3D10Predicate* p, WINBOOL) override
    {
        if (p)
            LogLimited("pred", "SetPredication is not supported");
    }

    // ---- rasterizer
    void STDMETHODCALLTYPE RSSetState(ID3D10RasterizerState* state) override
    {
        auto* r = static_cast<Raster12*>(state);
        if (r == m_raster)
            return;
        Bind(m_raster, r);
        m_psoDirty = true;
        m_scissorDirty = true;
    }

    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D10_VIEWPORT* vps) override
    {
        m_numViewports = std::min(n, 16u);
        for (UINT i = 0; i < m_numViewports; i++)
        {
            m_vp10[i] = vps[i];
            m_viewports[i] = {static_cast<float>(vps[i].TopLeftX), static_cast<float>(vps[i].TopLeftY), static_cast<float>(vps[i].Width),
                              static_cast<float>(vps[i].Height), vps[i].MinDepth, vps[i].MaxDepth};
        }
        m_viewportDirty = true;
        m_scissorDirty = true;
    }

    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D10_RECT* rects) override
    {
        m_numScissors = std::min(n, 16u);
        for (UINT i = 0; i < m_numScissors; i++)
            m_scissors[i] = rects[i];
        m_scissorDirty = true;
    }

    // ---- draws
    // Where the ocean's draws come (for leaving it out of the voxels): logged for one frame in 600.
    void NoteWaterDraw()
    {
        if (!m_vs || !m_vs->water || (m_giFrame % 600) != 7)
            return;
        Log("ocean draw at GI frame %llu: GI ran %d, rt0 format %d, scene depth %d, depth write %d, z-prepass recording %d",
            static_cast<unsigned long long>(m_giFrame), m_giLastRun == m_giFrame ? 1 : 0, m_rtv[0] ? static_cast<int>(m_rtv[0]->format) : -1,
            m_dsv && m_dsv->info == m_giSceneDepth ? 1 : 0, m_depth ? static_cast<int>(m_depth->d12.DepthWriteMask) : -1, VoxRecording() ? 1 : 0);
    }

    void STDMETHODCALLTYPE DrawIndexed(UINT count, UINT start, INT base) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(true))
        {
            m_list->DrawIndexedInstanced(count, 1, start, base, 0); m_stats.vertices += count;
            if (VoxRecording())
                RecordVoxDraw(false, true, count, 1, start, base, 0);
            else if (ShadowRecording())
                RecordVoxDraw(true, true, count, 1, start, base, 0);
            else if (CubeRecording())
                RecordVoxDraw(2, true, count, 1, start, base, 0);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE Draw(UINT count, UINT start) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(false))
        {
            m_list->DrawInstanced(count, 1, start, 0); m_stats.vertices += count;
            if (VoxRecording())
                RecordVoxDraw(false, false, count, 1, start, 0, 0);
            else if (ShadowRecording())
                RecordVoxDraw(true, false, count, 1, start, 0, 0);
            else if (CubeRecording())
                RecordVoxDraw(2, false, count, 1, start, 0, 0);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT count, UINT instances, UINT start, INT base, UINT startInstance) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(true))
        {
            m_list->DrawIndexedInstanced(count, instances, start, base, startInstance); m_stats.vertices += count * instances;
            if (VoxRecording())
                RecordVoxDraw(false, true, count, instances, start, base, startInstance);
            else if (ShadowRecording())
                RecordVoxDraw(true, true, count, instances, start, base, startInstance);
            else if (CubeRecording())
                RecordVoxDraw(2, true, count, instances, start, base, startInstance);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE DrawInstanced(UINT count, UINT instances, UINT start, UINT startInstance) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(false))
        {
            m_list->DrawInstanced(count, instances, start, startInstance); m_stats.vertices += count * instances;
            if (VoxRecording())
                RecordVoxDraw(false, false, count, instances, start, 0, startInstance);
            else if (ShadowRecording())
                RecordVoxDraw(true, false, count, instances, start, 0, startInstance);
            else if (CubeRecording())
                RecordVoxDraw(2, false, count, instances, start, 0, startInstance);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE DrawAuto() override
    {
        LogLimited("drawauto", "DrawAuto is not supported");
    }

    // Clear census: which targets are cleared to what, to give them matching optimized clear values (fast clears).
    std::map<std::string, uint64_t> m_clears;

    void NoteClear(const TextureCore* t, const FLOAT* color, float depth, UINT flagsAndStencil)
    {
        char key[160];
        if (color)
            snprintf(key, sizeof(key), "RT %ux%u fmt %d color %.3g %.3g %.3g %.3g", t->width, t->height, t->format, color[0], color[1], color[2], color[3]);
        else
            snprintf(key, sizeof(key), "DS %ux%u fmt %d flags %u depth %.3g stencil %u", t->width, t->height, t->format, flagsAndStencil & 0xFF, depth,
                     flagsAndStencil >> 8);
        m_clears[key]++;
    }

    // Barrier census: transitions by resource and states.
    std::map<std::string, uint64_t> m_barrierKinds;

    void NoteBarrier(const ResourceInfo* info, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        char key[160];
        if (info)
        {
            const auto* t = static_cast<const TextureCore*>(info);
            snprintf(key, sizeof(key), "tex %ux%u fmt %d bind 0x%x: 0x%x -> 0x%x", t->width, t->height, t->format, t->bind, before, after);
        }
        else
            snprintf(key, sizeof(key), "buffer page: 0x%x -> 0x%x", before, after);
        m_barrierKinds[key]++;
    }

    // A draw packet (DrawPacket.h; CryMT's own per-object loops): its constants go to frame upload memory and replace
    // the bound buffer of their slot for this draw only. The slot's binding itself stays: its applied address no longer
    // matches, so the next draw applies the bound buffer again.
    void DrawIndexedWithConstants(const uint8_t* packet)
    {
        DrawPacketHeader h;
        memcpy(&h, packet, sizeof(h));
        m_stats.packets++;
        // The packet's own streams and index buffer, after its constants; the bindings they replace come back after.
        const uint8_t* q = packet + sizeof(h);
        for (uint32_t i = 0; i < h.constants; i++)
        {
            DrawPacketConstants c;
            memcpy(&c, q, sizeof(c));
            q += sizeof(c) + c.vectors * 16u;
        }
        struct Saved
        {
            UINT slot;
            ID3D10Buffer* buffer;
            UINT stride, offset;
        } saved[kDrawPacketMaxStreams];
        UINT savedCount = 0;
        for (uint32_t i = 0; i < h.streams && savedCount < kDrawPacketMaxStreams; i++)
        {
            DrawPacketStream s;
            memcpy(&s, q, sizeof(s));
            q += sizeof(s);
            if (s.slot >= 16)
                continue;
            VertexBinding& vb = m_vb[s.slot];
            saved[savedCount] = {s.slot, vb.buffer, vb.stride, vb.offset};
            if (vb.buffer)
                vb.buffer->AddRef();
            savedCount++;
            ID3D10Buffer* b = reinterpret_cast<ID3D10Buffer*>(s.buffer);
            IASetVertexBuffers(s.slot, 1, &b, &s.stride, &s.offset);
        }
        ID3D10Buffer* savedIb = nullptr;
        DXGI_FORMAT savedIbFormat = m_ibFormat;
        UINT savedIbOffset = m_ibOffset;
        if (h.indexBuffer)
        {
            DrawPacketIndex x;
            memcpy(&x, q, sizeof(x));
            savedIb = m_ib;
            if (savedIb)
                savedIb->AddRef();
            IASetIndexBuffer(reinterpret_cast<ID3D10Buffer*>(x.buffer), static_cast<DXGI_FORMAT>(x.format), x.offset);
        }
        for (int attempt = 0; attempt < 3; attempt++)
        {
            const uint64_t serial = g_mainSerial;
            if (!Apply(true))
                break;
            D3D12_GPU_VIRTUAL_ADDRESS va[kDrawPacketMaxConstants];
            Stage stage[kDrawPacketMaxConstants];
            UINT slot[kDrawPacketMaxConstants];
            UINT n = 0;
            const uint8_t* p = packet + sizeof(h);
            for (uint32_t i = 0; i < h.constants && i < kDrawPacketMaxConstants; i++)
            {
                DrawPacketConstants c;
                memcpy(&c, p, sizeof(c));
                const uint32_t bytes = c.vectors * 16u;
                const Stage s = c.stage == 0 ? kVS : c.stage == 1 ? kPS : kGS;
                if (bytes && c.slot < kRootCbSlots[s])
                {
                    const UploadAlloc up = FrameUpload((bytes + 255) & ~uint64_t{255}, 256);
                    if (up.cpu)
                    {
                        memcpy(up.cpu, p + sizeof(c), bytes);
                        va[n] = up.res->GetGPUVirtualAddress() + up.offset;
                        stage[n] = s;
                        slot[n] = c.slot;
                        n++;
                    }
                }
                p += sizeof(c) + bytes;
            }
            if (serial != g_mainSerial)
                continue; // an upload submitted the list: apply again in the new one
            for (UINT i = 0; i < n; i++)
            {
                m_list->SetGraphicsRootConstantBufferView(kRootCb[stage[i]] + slot[i], va[i]);
                m_stage[stage[i]].cbApplied[slot[i]] = va[i];
            }
            m_list->DrawIndexedInstanced(h.count, 1, h.start, h.base, 0);
            if (VoxRecording())
                RecordVoxDraw(false, true, h.count, 1, h.start, h.base, 0);
            else if (ShadowRecording())
                RecordVoxDraw(true, true, h.count, 1, h.start, h.base, 0);
            else if (CubeRecording())
                RecordVoxDraw(2, true, h.count, 1, h.start, h.base, 0);
            break;
        }
        for (UINT i = 0; i < savedCount; i++)
        {
            IASetVertexBuffers(saved[i].slot, 1, &saved[i].buffer, &saved[i].stride, &saved[i].offset);
            if (saved[i].buffer)
                saved[i].buffer->Release();
        }
        if (h.indexBuffer)
        {
            IASetIndexBuffer(savedIb, savedIbFormat, savedIbOffset);
            if (savedIb)
                savedIb->Release();
        }
        AfterDraw();
    }

    // The engine's object motion blur target is reused soon after its pass: at the overview spot (19 motion blur draws a
    // frame into their own RGBA16F target) the target held a copy of the scene colour by the time the scene was handed
    // to DLSS, so RunMotion read colours as velocities (in 44% of the frames; its distance check threw most away, which
    // also lost the real velocities - the "vegetation garbage" of 2026-11-05). The first draw, clear or copy after the
    // pass (or the hand-over) copies the target into m_ombCopy, which RunMotion and the CRYMT_MVSEQ dumps read.
    void KeepObjectMotion()
    {
        if (!m_ombPending || (m_vs && m_vs->motionBlur))
            return;
        m_ombPending = false;
        TextureCore* src = m_ombTarget;
        if (!src || !src->res || src->format != DXGI_FORMAT_R16G16B16A16_FLOAT || src->samples != 1 || src->dim != D3D10_RESOURCE_DIMENSION_TEXTURE2D)
            return;
        if (!m_ombCopy.res || m_ombCopy.width != src->width || m_ombCopy.height != src->height)
        {
            DeferRelease(m_ombCopy.res);
            m_ombCopy.res = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = src->width;
            d.Height = src->height;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_ombCopy.res))))
            {
                m_ombCopy.res = nullptr;
                m_ombCopy.width = m_ombCopy.height = 0;
                return;
            }
            m_ombCopy.dim = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            m_ombCopy.width = src->width;
            m_ombCopy.height = src->height;
            m_ombCopy.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_ombCopy.samples = 1;
            m_ombCopy.state = D3D12_RESOURCE_STATE_COPY_DEST;
            Log("object motion: the motion blur target kept in a copy, %ux%u", src->width, src->height);
        }
        const D3D12_RESOURCE_STATES before = src->state;
        Require(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Require(&m_ombCopy, D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        m_list->CopyResource(m_ombCopy.res, src->res);
        Require(src, before);
        FlushBarriers();
        m_ombCopyFrame = m_frame;
        m_stats.objectMotionCopies++;
    }

    void AfterDraw()
    {
        if (m_vs && m_vs->motionBlur && m_rtv[0])
        {
            m_ombTarget = static_cast<TextureCore*>(m_rtv[0]->info);
            m_ombFrame = m_frame;
            m_ombPending = true;
            // Diagnosis (2026-11-13: at the overview spot the "motion blur target" held the scene colour): the draws with
            // MotionBlurData in the vertex shader this frame, by target and shaders.
            if (m_ombDiagFrame != m_frame)
            {
                m_ombDiagFrame = m_frame;
                m_ombDiagCount = 0;
            }
            if (m_ombDiagCount < 8)
                m_ombDiag[m_ombDiagCount++] = {m_ombTarget, m_vs->contentHash, m_ps ? m_ps->contentHash : 0};
            else
                m_ombDiagCount++;
        }
        m_stats.draws++;
        if (m_engineCubeRes && m_rtv[0] && m_rtv[0]->resource == m_engineCubeRes)
        {
            m_stats.engineCubeDraws++;
            const D3D10_RENDER_TARGET_VIEW_DESC& d = m_rtv[0]->desc;
            const UINT face = d.ViewDimension == D3D10_RTV_DIMENSION_TEXTURE2DARRAY ? d.Texture2DArray.FirstArraySlice : 0;
            m_stats.engineCubeFaces[face < 6 ? face : 5]++;
            m_engineCubeLastFace = static_cast<int>(face < 6 ? face : 5);
            if (m_engineCubeDumpWanted)
            {
                // The capture key: what the next draw into the cube (the engine's Encode_ToLDR) read, as <tag>_envsrc.
                m_engineCubeDumpWanted = false;
                for (int slot = 0; slot < 4; slot++)
                    if (Srv12* s = m_stage[kPS].srv[slot]; s && s->info && !s->info->buffer)
                    {
                        const D3D12_RESOURCE_DESC rd = s->info->res->GetDesc();
                        Log("engine cube draw (face %u): PS slot %d %llux%u format %d array %u", face, slot,
                            static_cast<unsigned long long>(rd.Width), rd.Height, static_cast<int>(rd.Format), rd.DepthOrArraySize);
                        if (slot == 0)
                        {
                            Require(s->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                            FlushBarriers();
                            RecordDump(s->info->res, "envsrc", 0);
                            Require(s->info, kSrvState);
                            FlushBarriers();
                        }
                    }
            }
        }
        if (++m_drawsSinceSubmit >= 2048 && !m_activeQueries)
            Submit();
    }

    // ---- clears, copies, updates
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D10RenderTargetView* view, const FLOAT color[4]) override
    {
        KeepObjectMotion();
        auto* v = static_cast<Rtv12*>(view);
        if (!v)
            return;
        Require(v->info, D3D12_RESOURCE_STATE_RENDER_TARGET);
        FlushBarriers();
        m_list->ClearRenderTargetView(v->cpu, color, 0, nullptr);
        m_stats.rtClears++;
        NoteClear(static_cast<TextureCore*>(v->info), color, 0.0f, 0);
    }

    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D10DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil) override
    {
        auto* v = static_cast<Dsv12*>(view);
        if (!v)
            return;
        Require(v->info, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        FlushBarriers();
        m_list->ClearDepthStencilView(v->cpu, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil, 0, nullptr);
        m_stats.dsClears++;
        NoteClear(static_cast<TextureCore*>(v->info), nullptr, depth, flags | stencil << 8);
    }

    void STDMETHODCALLTYPE CopyResource(ID3D10Resource* dst, ID3D10Resource* src) override
    {
        KeepObjectMotion();
        if (!dst || !src)
            return;
        Buffer12* db = AsBuffer(dst);
        Buffer12* sb = AsBuffer(src);
        if (db && sb)
        {
            CopyBuffer(db, 0, sb, 0, std::min(db->desc.ByteWidth, sb->desc.ByteWidth));
            return;
        }
        TextureCore* dt = AsTexture(dst);
        TextureCore* st = AsTexture(src);
        if (!m_pastScene && dt && st && LikeSceneColour(st) && LikeSceneColour(dt))
            NotePreCopy(dt); // a copy of the scene colour before DLSS (jittered)
        if (!dt || !st)
            return;
        if (dt->Staging() || st->Staging())
        {
            for (UINT sub = 0; sub < dt->subresources && sub < st->subresources; sub++)
                CopyTexture(dt, sub, 0, 0, 0, st, sub, nullptr);
            return;
        }
        Require(dt, D3D12_RESOURCE_STATE_COPY_DEST);
        Require(st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        FlushBarriers();
        m_list->CopyResource(dt->res, st->res);
    }

    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D10Resource* dst, UINT dstSub, UINT x, UINT y, UINT z, ID3D10Resource* src, UINT srcSub,
                                                 const D3D10_BOX* box) override
    {
        KeepObjectMotion();
        if (!dst || !src)
            return;
        Buffer12* db = AsBuffer(dst);
        Buffer12* sb = AsBuffer(src);
        if (db && sb)
        {
            const UINT from = box ? box->left : 0;
            const UINT bytes = box ? box->right - box->left : sb->desc.ByteWidth;
            CopyBuffer(db, x, sb, from, bytes);
            return;
        }
        TextureCore* dt = AsTexture(dst);
        TextureCore* st = AsTexture(src);
        if (dt && st)
            CopyTexture(dt, dstSub, x, y, z, st, srcSub, box);
    }

    void STDMETHODCALLTYPE UpdateSubresource(ID3D10Resource* dst, UINT sub, const D3D10_BOX* box, const void* data, UINT rowPitch, UINT depthPitch) override;

    void STDMETHODCALLTYPE GenerateMips(ID3D10ShaderResourceView*) override
    {
        LogLimited("mips", "GenerateMips is not implemented");
    }

    void STDMETHODCALLTYPE ResolveSubresource(ID3D10Resource* dst, UINT dstSub, ID3D10Resource* src, UINT srcSub, DXGI_FORMAT format) override
    {
        KeepObjectMotion();
        TextureCore* dt = AsTexture(dst);
        TextureCore* st = AsTexture(src);
        if (!dt || !st)
            return;
        Require(dt, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        Require(st, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        FlushBarriers();
        m_list->ResolveSubresource(dt->res, dstSub, st->res, srcSub, format);
    }

    // ---- getters
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT start, UINT n, ID3D10Buffer** out) override
    {
        GetArray(reinterpret_cast<ID3D10Buffer* const*>(m_stage[kVS].cb), start, n, out, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT start, UINT n, ID3D10Buffer** out) override
    {
        GetArray(reinterpret_cast<ID3D10Buffer* const*>(m_stage[kGS].cb), start, n, out, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT start, UINT n, ID3D10Buffer** out) override
    {
        GetArray(reinterpret_cast<ID3D10Buffer* const*>(m_stage[kPS].cb), start, n, out, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    }
    void GetViews(Stage s, UINT start, UINT n, ID3D10ShaderResourceView** out)
    {
        for (UINT i = 0; i < n; i++)
        {
            Srv12* v = start + i < D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ? m_stage[s].srv[start + i] : nullptr;
            if (v)
                v->AddRef();
            out[i] = v;
        }
    }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView** out) override
    {
        GetViews(kVS, start, n, out);
    }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView** out) override
    {
        GetViews(kGS, start, n, out);
    }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView** out) override
    {
        GetViews(kPS, start, n, out);
    }
    void STDMETHODCALLTYPE VSGetSamplers(UINT start, UINT n, ID3D10SamplerState** out) override
    {
        GetArray(reinterpret_cast<ID3D10SamplerState* const*>(m_stage[kVS].sampler), start, n, out, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE GSGetSamplers(UINT start, UINT n, ID3D10SamplerState** out) override
    {
        GetArray(reinterpret_cast<ID3D10SamplerState* const*>(m_stage[kGS].sampler), start, n, out, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE PSGetSamplers(UINT start, UINT n, ID3D10SamplerState** out) override
    {
        GetArray(reinterpret_cast<ID3D10SamplerState* const*>(m_stage[kPS].sampler), start, n, out, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT);
    }
    template <class T, class U>
    static void GetOne(T* v, U** out)
    {
        if (v)
            v->AddRef();
        *out = v;
    }
    void STDMETHODCALLTYPE VSGetShader(ID3D10VertexShader** out) override
    {
        GetOne(m_vs, out);
    }
    void STDMETHODCALLTYPE GSGetShader(ID3D10GeometryShader** out) override
    {
        GetOne(m_gs, out);
    }
    void STDMETHODCALLTYPE PSGetShader(ID3D10PixelShader** out) override
    {
        GetOne(m_ps, out);
    }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D10InputLayout** out) override
    {
        GetOne(m_layout, out);
    }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT start, UINT n, ID3D10Buffer** buffers, UINT* strides, UINT* offsets) override
    {
        for (UINT i = 0; i < n; i++)
        {
            const VertexBinding none{};
            const VertexBinding& vb = start + i < 16 ? m_vb[start + i] : none;
            if (buffers)
                GetOne(vb.buffer, &buffers[i]);
            if (strides)
                strides[i] = vb.stride;
            if (offsets)
                offsets[i] = vb.offset;
        }
    }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D10Buffer** buffer, DXGI_FORMAT* format, UINT* offset) override
    {
        if (buffer)
            GetOne(m_ib, buffer);
        if (format)
            *format = m_ibFormat;
        if (offset)
            *offset = m_ibOffset;
    }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY* t) override
    {
        *t = m_topology;
    }
    void STDMETHODCALLTYPE GetPredication(ID3D10Predicate** p, WINBOOL* value) override
    {
        if (p)
            *p = nullptr;
        if (value)
            *value = FALSE;
    }
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT n, ID3D10RenderTargetView** views, ID3D10DepthStencilView** depth) override
    {
        for (UINT i = 0; views && i < n; i++)
            GetOne(i < 8 ? m_rtv[i] : nullptr, &views[i]);
        if (depth)
            GetOne(m_dsv, depth);
    }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D10BlendState** state, FLOAT factor[4], UINT* mask) override
    {
        if (state)
        {
            ID3D10BlendState1* b = m_blend;
            GetOne(b, reinterpret_cast<ID3D10BlendState1**>(state));
        }
        if (factor)
            memcpy(factor, m_blendFactor, sizeof(m_blendFactor));
        if (mask)
            *mask = m_sampleMask;
    }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D10DepthStencilState** state, UINT* ref) override
    {
        if (state)
            GetOne(m_depth, state);
        if (ref)
            *ref = m_stencilRef;
    }
    void STDMETHODCALLTYPE SOGetTargets(UINT n, ID3D10Buffer** buffers, UINT* offsets) override
    {
        for (UINT i = 0; i < n; i++)
        {
            if (buffers)
                buffers[i] = nullptr;
            if (offsets)
                offsets[i] = 0;
        }
    }
    void STDMETHODCALLTYPE RSGetState(ID3D10RasterizerState** state) override
    {
        GetOne(m_raster, state);
    }
    void STDMETHODCALLTYPE RSGetViewports(UINT* n, D3D10_VIEWPORT* vps) override
    {
        if (!vps)
        {
            *n = m_numViewports;
            return;
        }
        for (UINT i = 0; i < *n; i++)
            vps[i] = i < m_numViewports ? m_vp10[i] : D3D10_VIEWPORT{};
    }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* n, D3D10_RECT* rects) override
    {
        if (!rects)
        {
            *n = m_numScissors;
            return;
        }
        for (UINT i = 0; i < *n; i++)
            rects[i] = i < m_numScissors ? m_scissors[i] : D3D10_RECT{};
    }

    // ---- device misc
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override
    {
        return g.dev->GetDeviceRemovedReason();
    }
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT flags) override
    {
        m_exceptionMode = flags;
        return S_OK;
    }
    UINT STDMETHODCALLTYPE GetExceptionMode() override
    {
        return m_exceptionMode;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        return PrivateGet(this, guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        return PrivateSet(this, guid, size, data, nullptr);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
    {
        return PrivateSet(this, guid, 0, nullptr, data);
    }
    void STDMETHODCALLTYPE ClearState() override;
    void STDMETHODCALLTYPE Flush() override
    {
        Submit();
    }

    // ---- creation
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D10_BUFFER_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Buffer** out) override;
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D10_TEXTURE1D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture1D** out) override;
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D10_TEXTURE2D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture2D** out) override;
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D10_TEXTURE3D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture3D** out) override;
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D10Resource* r, const D3D10_SHADER_RESOURCE_VIEW_DESC* desc, ID3D10ShaderResourceView** out) override
    {
        D3D10_SHADER_RESOURCE_VIEW_DESC1 d1{};
        if (desc)
            memcpy(&d1, desc, sizeof(*desc));
        return CreateShaderResourceView1(r, desc ? &d1 : nullptr, reinterpret_cast<ID3D10ShaderResourceView1**>(out));
    }
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(ID3D10Resource* r, const D3D10_SHADER_RESOURCE_VIEW_DESC1* desc, ID3D10ShaderResourceView1** out) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D10Resource* r, const D3D10_RENDER_TARGET_VIEW_DESC* desc, ID3D10RenderTargetView** out) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D10Resource* r, const D3D10_DEPTH_STENCIL_VIEW_DESC* desc, ID3D10DepthStencilView** out) override;
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D10_INPUT_ELEMENT_DESC* elements, UINT n, const void* code, SIZE_T bytes, ID3D10InputLayout** out) override;
    template <class S, class I>
    HRESULT CreateShader(const void* code, SIZE_T bytes, I** out)
    {
        if (!code || !bytes)
            return E_INVALIDARG;
        if (!out)
            return S_FALSE;
        auto* s = new S;
        s->code.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + bytes);
        s->contentHash = HashBytes(code, bytes);
        s->Reflect();
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* code, SIZE_T bytes, ID3D10VertexShader** out) override
    {
        return CreateShader<VS12>(code, bytes, out);
    }
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* code, SIZE_T bytes, ID3D10GeometryShader** out) override
    {
        return CreateShader<GS12>(code, bytes, out);
    }
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D10_SO_DECLARATION_ENTRY*, UINT, UINT,
                                                                   ID3D10GeometryShader**) override
    {
        LogLimited("gsso", "CreateGeometryShaderWithStreamOutput is not supported");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* code, SIZE_T bytes, ID3D10PixelShader** out) override
    {
        return CreateShader<PS12>(code, bytes, out);
    }
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D10_BLEND_DESC* desc, ID3D10BlendState** out) override
    {
        if (!desc)
            return E_INVALIDARG;
        const D3D10_BLEND_DESC1 d1 = Blend1From(*desc);
        return CreateBlendState1(&d1, reinterpret_cast<ID3D10BlendState1**>(out));
    }
    HRESULT STDMETHODCALLTYPE CreateBlendState1(const D3D10_BLEND_DESC1* desc, ID3D10BlendState1** out) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D10_DEPTH_STENCIL_DESC* desc, ID3D10DepthStencilState** out) override;
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D10_RASTERIZER_DESC* desc, ID3D10RasterizerState** out) override;
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D10_SAMPLER_DESC* desc, ID3D10SamplerState** out) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D10_QUERY_DESC* desc, ID3D10Query** out) override;
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D10_QUERY_DESC*, ID3D10Predicate**) override
    {
        LogLimited("predicate", "CreatePredicate is not supported");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D10_COUNTER_DESC*, ID3D10Counter**) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT format, UINT* support) override
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        if (!support || FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
            return E_FAIL;
        *support = fs.Support1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels) override
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
        if (!levels)
            return E_INVALIDARG;
        *levels = SUCCEEDED(g.dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) ? q.NumQualityLevels : 0;
        return S_OK;
    }
    void STDMETHODCALLTYPE CheckCounterInfo(D3D10_COUNTER_INFO* info) override
    {
        if (info)
            *info = {};
    }
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D10_COUNTER_DESC*, D3D10_COUNTER_TYPE*, UINT*, char*, UINT*, char*, UINT*, char*, UINT*) override
    {
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetCreationFlags() override
    {
        return m_creationFlags;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE, REFIID, void**) override
    {
        LogLimited("shared", "OpenSharedResource is not supported");
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE SetTextFilterSize(UINT w, UINT h) override
    {
        m_textFilter[0] = w;
        m_textFilter[1] = h;
    }
    void STDMETHODCALLTYPE GetTextFilterSize(UINT* w, UINT* h) override
    {
        if (w)
            *w = m_textFilter[0];
        if (h)
            *h = m_textFilter[1];
    }
    D3D10_FEATURE_LEVEL1 STDMETHODCALLTYPE GetFeatureLevel() override
    {
        return D3D10_FEATURE_LEVEL_10_1;
    }

    // ---- translation
    void Submit();
    void BeginList();
    void RestoreListState();
    // Pass names for frame debuggers (CRYMT_RENDERDOC or CRYMT_MARKERS): PIX events on the main list; names still open
    // at a submission are closed there and opened again on the next list.
    void PassName(const char* text);
    void PassNameEnd();
    void WaitIdle();
    bool Apply(bool indexed);
    bool ApplyOnce(bool indexed);
    ID3D12PipelineState* Pso(bool indexed);
    void ApplyStage(Stage s, uint32_t cbMask, UINT srvEnd, UINT samplerEnd);

    void Require(ResourceInfo* info, D3D12_RESOURCE_STATES needed)
    {
        if (info->buffer || info->state == needed)
            return;
        if (info->state == D3D12_RESOURCE_STATE_DEPTH_WRITE && needed == kSrvState)
            m_stats.depthToSrv++;
        NoteBarrier(info, info->state, needed);
        m_barriers.push_back(Transition(info->res, info->state, needed));
        info->state = needed;
    }

    void FlushBarriers()
    {
        if (m_barriers.empty())
            return;
        m_list->ResourceBarrier(static_cast<UINT>(m_barriers.size()), m_barriers.data());
        m_stats.barriers += m_barriers.size();
        m_stats.barrierCalls++;
        m_barriers.clear();
    }

    // A default buffer's page is read by this list. Pages start each list in COMMON (buffers decay at the end of every
    // ExecuteCommandLists) and are moved to GENERIC_READ explicitly, so a later copy into them knows the state to leave.
    void MarkRead(Buffer12* b)
    {
        if (BufferPage* p = b->Page())
            MarkPageRead(p);
    }

    void MarkPageRead(BufferPage* p)
    {
        if (p->listSerial != g_mainSerial)
        {
            p->listSerial = g_mainSerial;
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_GENERIC_READ));
            p->listState = kPageAllRead;
        }
        else if (p->listState == kPageCopyDest)
        {
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));
            p->listState = kPageAllRead;
        }
    }

    void PrepareCopyDest(BufferPage* p)
    {
        if (p->listSerial != g_mainSerial)
        {
            p->listSerial = g_mainSerial;
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
            p->listState = kPageCopyDest;
        }
        else if (p->listState == kPageAllRead)
        {
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST));
            p->listState = kPageCopyDest;
        }
    }

    void CopyBuffer(Buffer12* dst, uint64_t dstOffset, Buffer12* src, uint64_t srcOffset, uint64_t bytes)
    {
        if (dst->Dynamic() || !dst->Res() || !src->Res() || !bytes)
        {
            LogLimited("copybuf", "buffer copy into a dynamic buffer is not supported");
            return;
        }
        if (dst->Page() && dst->Page() == src->Page())
        {
            LogLimited("copysame", "buffer copy within one page is not supported");
            return;
        }
        if (BufferPage* p = dst->Page())
            PrepareCopyDest(p);
        if (dst->Staging())
            dst->gpuWriteSerial = g_mainSerial;
        MarkRead(src);
        if (src->Staging())
            src->gpuReadSerial = g_mainSerial;
        FlushBarriers();
        m_list->CopyBufferRegion(dst->Res(), dst->Offset() + dstOffset, src->Res(), src->Offset() + srcOffset, bytes);
    }

    D3D12_TEXTURE_COPY_LOCATION Location(TextureCore* t, UINT sub)
    {
        D3D12_TEXTURE_COPY_LOCATION l{};
        l.pResource = t->res;
        if (t->Staging())
        {
            l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            l.PlacedFootprint = t->footprints[sub];
        }
        else
        {
            l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            l.SubresourceIndex = sub;
        }
        return l;
    }

    void CopyTexture(TextureCore* dst, UINT dstSub, UINT x, UINT y, UINT z, TextureCore* src, UINT srcSub, const D3D10_BOX* box)
    {
        if (dstSub >= dst->subresources || srcSub >= src->subresources)
            return;
        // An empty box copies nothing in D3D10; D3D12 calls it invalid (undefined behaviour). The texture streamer sends
        // such copies for BC textures (256x256 fmt 77, region 0x0x0 in the copy journal of a GPU hang, 2026-11-12).
        if (box && (box->right <= box->left || box->bottom <= box->top || box->back <= box->front))
        {
            g_emptyCopies++;
            return;
        }
        if (dst->Staging())
            dst->gpuWriteSerial = g_mainSerial;
        else
            Require(dst, D3D12_RESOURCE_STATE_COPY_DEST);
        if (src->Staging())
            src->gpuReadSerial = g_mainSerial;
        else
            Require(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        FlushBarriers();
        const D3D12_TEXTURE_COPY_LOCATION d = Location(dst, dstSub);
        const D3D12_TEXTURE_COPY_LOCATION s = Location(src, srcSub);
        if (dst->Staging() && src->Staging())
        {
            CopyStagingRows(dst, dstSub, x, y, z, src, srcSub, box);
            return;
        }
        NoteCopy("CopyTexture", d, x, y, z, s, reinterpret_cast<const D3D12_BOX*>(box));
        m_list->CopyTextureRegion(&d, x, y, z, &s, reinterpret_cast<const D3D12_BOX*>(box));
    }

    // A copy between two staging textures. Both are buffers here (each subresource at its copyable footprint), and D3D12's
    // CopyTextureRegion needs a texture on one side: footprint to footprint is invalid. The game's save thumbnail does
    // this (the screen read back into a staging texture, then a 256x72 region into another): the GPU hung on it, the
    // driver reset it (DRED and the copy journal of the user's hang 2026-11-12 19:14 and of rp_full_safe 15:27, both a
    // 256x72 footprint-to-footprint copy as the last one). So the region goes row by row with CopyBufferRegion (the
    // staging heap is custom, system memory: a valid copy source and destination).
    void CopyStagingRows(TextureCore* dst, UINT dstSub, UINT x, UINT y, UINT z, TextureCore* src, UINT srcSub, const D3D10_BOX* box)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& df = dst->footprints[dstSub];
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& sf = src->footprints[srcSub];
        const int f = sf.Footprint.Format;
        const bool bc = (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
        const UINT block = bc ? 4 : 1;
        const UINT bytes = bc ? ((f >= 70 && f <= 72) || (f >= 79 && f <= 81) ? 8 : 16) : FormatBytes(sf.Footprint.Format);
        const UINT left = box ? box->left : 0, top = box ? box->top : 0, front = box ? box->front : 0;
        const UINT right = box ? box->right : sf.Footprint.Width, bottom = box ? box->bottom : sf.Footprint.Height;
        const UINT back = box ? box->back : sf.Footprint.Depth;
        const UINT cols = (right - left + block - 1) / block, rows = (bottom - top + block - 1) / block;
        const UINT64 rowBytes = static_cast<UINT64>(cols) * bytes;
        const UINT srcRows = src->rows[srcSub], dstRows = dst->rows[dstSub]; // rows (of blocks) per slice
        UINT copies = 0;
        for (UINT slice = 0; slice < back - front; slice++)
            for (UINT r = 0; r < rows; r++)
            {
                const UINT sy = top / block + r, dy = y / block + r;
                if (sy >= srcRows || dy >= dstRows)
                    break;
                const UINT64 so = sf.Offset + (static_cast<UINT64>(front + slice) * srcRows + sy) * sf.Footprint.RowPitch + (left / block) * bytes;
                const UINT64 dO = df.Offset + (static_cast<UINT64>(z + slice) * dstRows + dy) * df.Footprint.RowPitch + (x / block) * bytes;
                const UINT64 room = std::min<UINT64>(sf.Footprint.RowPitch - (left / block) * bytes, df.Footprint.RowPitch - (x / block) * bytes);
                m_list->CopyBufferRegion(dst->res, dO, src->res, so, std::min(rowBytes, room));
                copies++;
            }
        g_stagingRowCopies += copies;
        LogLimited("stagingcopy", "staging to staging copy %ux%ux%u (format %d) as %u row copies", right - left, bottom - top, back - front, f, copies);
    }

    // Upload memory for this list (UpdateSubresource, dynamic textures).
    UploadAlloc FrameUpload(uint64_t bytes, uint64_t align)
    {
        if (bytes > m_frameRing.size / 4)
        {
            UploadAlloc a;
            a.res = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (a.res)
            {
                a.cpu = MapWhole(a.res);
                DeferRelease(a.res);
            }
            return a;
        }
        for (;;)
        {
            UploadAlloc a;
            if (m_frameRing.TryAlloc(bytes, align, a))
                return a;
            Submit();
            WaitFence(m_frameRing.OldestFence() ? m_frameRing.OldestFence() : g.signaled);
            PollFence();
            m_frameRing.Reclaim();
        }
    }

    void UploadPending(TextureCore* t, UINT sub, const TextureCore::Pending& p)
    {
        Require(t, D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = t->res;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = sub;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = p.up.res;
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = p.footprint;
        NoteCopy("UploadPending", d, 0, 0, 0, s, nullptr);
        m_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    }

    // ---- queries
    void QueryBegin(Query12* q);
    void QueryEnd(Query12* q);
    HRESULT QueryData(Query12* q, void* data, UINT size, UINT flags);
    HRESULT QueryPeek(Query12* q, void* data, UINT size, UINT flags, uint64_t* flushSerial);
    HRESULT QueryRead(Query12* q, void* data, UINT size);
    void QueryFree(Query12* q);

    // ---- presentation
    HRESULT PresentFrame(IDXGISwapChain3* chain, ID3D12Resource* target, TextureCore* backbuffer, UINT sync, UINT flags);

    // ---- DLSS: the scene is complete, before HDR post-processing. The bound render target is the scene's HDR color
    // (RGBA16F) and the bound depth stencil its depth; DLAA's output replaces the color.
    // ---- CryMT GI, screen space (CRYMT_GI) ------------------------------------------------------------------------
    // At SceneDone the HDR scene colour is copied (the bounce source for the next frame) and the projection kept. The
    // next frame's first draw that reads the GI placeholder (general pass, after the z-prepass) runs a compute pass over
    // the scene depth: per half-resolution pixel, horizons along 4 screen directions (both ways) within a radius in
    // metres give the sky occlusion (a: share of the engine's flat ambient removed) and the light seen above the
    // horizon (rgb, cosine weighted), blended with the previous result. Pixel shaders then see this texture in place
    // of the placeholder. Own design after the horizon-based AO/indirect-light literature.
    struct GiSettings
    {
        bool on = false;
        float temporal = 0.75f, maxDistance = 120.0f, debug = 0.0f;
        bool upsample = true;    // CRYMT_GI_UPSAMPLE=0: the engine reads the half-size GI directly (v1.3)
        bool fixedNoise = false; // CRYMT_GI_FIXEDNOISE=1: the same sample pattern every frame
        bool nearest = false;    // CRYMT_GI_NEAREST=1: a GI texel stands for the nearest depth of its 2x2 block
        bool narrowNormals = false; // CRYMT_GI_NARROWNORMALS=1: normals one GI texel wide only (faces show on round meshes)
        bool weaponGi = true;       // CRYMT_GI_WEAPON=0: no GI on the first-person view (weapon, hands, held objects)
        float occFar = 16;          // CRYMT_GI_OCCRANGE (m): the cones' occlusion full to half of it, gone at it (0: no limit)
        float occStrength = 1;      // CRYMT_GI_OCCSTRENGTH: occlusion strength under cover (Num4 / Num7 multiply it)
        bool voxels = false;     // CRYMT_GI_VOXELS=1: the voxel grid around the camera (kVoxShader)
        float voxelSize = 0.25f; // CRYMT_GI_VOXELSIZE, metres (the finest level)
        float voxelView = 0;     // CRYMT_GI_VOXELVIEW: 1 wire faces over the frame + GI inset, 2 solid voxels (implies VOXELS)
        float voxelMix = 0;      // CRYMT_GI_VOXELS value: 1 the cones through the grid (the GI), 4 the grid without cones (cost)
        float voxelStrength = 1.5f, voxelReach = 64.0f; // CRYMT_GI_VOXELSTRENGTH (bounce light of the cones), CRYMT_GI_VOXELREACH (m)
        float voxelOcclusion = 0.6f; // CRYMT_GI_VOXELAO: how much of the flat ambient the cones' occlusion removes
        bool geometry = true;        // the z-prepass draws voxelized into level 0 (RunVoxelGeometry); CRYMT_GI_GEOMETRY=0 off
                                     // (default on since 2026-11-06, the user's choice: ~5-7 FPS)
        bool geometryFoliage = false; // CRYMT_GI_GEOMETRY=2: foliage too (mostly open cells)
        bool geometryPersist = true;  // CRYMT_GI_GEOMPERSIST=0: voxelized geometry only from the last three voxelizations
        bool opacityGeometry = true;  // the cells' opacity from the voxelized geometry only, the screen gives only their colour
                                      // (levels with geometry; the coarsest keeps the screen); CRYMT_GI_OCCUPANCY=screen: also
                                      // from the screen (before 2026-11-10: 12 x the cells coming and going on a still camera)
        float foliageOpacity = 0.5f;  // CRYMT_GI_FOLIAGE: an alpha-tested material's cell opacity (by leaf shape with materials)
        bool materials = true;        // CRYMT_GI_MATERIALS=0: geometry without textures (no albedo, no alpha test)
        float coneLift = 0.5f, coneStart = 1; // CRYMT_GI_CONELIFT, CRYMT_GI_CONESTART: the near cones', cells of level 0
        float nearOcclusion = 0;           // CRYMT_GI_NEAROCC: strength of the near occlusion cones (0 none; Num3 sets 1)
        float sunAlbedo = 0.3f;            // CRYMT_GI_SUNALBEDO: the albedo assumed for sunlit geometry cells (0: none)
        float skyFill = 0.15f;             // CRYMT_GI_SKYFILL: their light from the sky, a share of the sun's
        int sunMarch = 128;                // CRYMT_GI_SUNMARCH: cells the light pass marches towards the sun in a level, up to
                                           // 128 (the grid). 24 before 2026-11-13 (6 m at level 0): mine at noon, the light
                                           // change of cells coming into view (inside, level 1) mean 12.9 -> 4.7 / 255, > 48:
                                           // 16% -> 2% of cells; +0.05 ms
        bool shadowGeometry = false;       // CRYMT_GI_SHADOWGEOM=1: also the shadow maps' casters (off: their vertex shaders
                                           // put the depth elsewhere, none of their draws landed in the grid - a capture)
        bool cubeGeometry = false;         // CRYMT_GI_CUBEGEOM=1: the engine cube's face draws (solid only) into the geometry
                                           // levels, their stamps to fill what the camera does not see. OFF: it does not work
                                           // (2026-11-13, mine): the general pass places objects by a per-object matrix that
                                           // already holds the camera, so replacing the frame's view-projection left them in the
                                           // face's perspective - streaks through the whole grid (voxsnap_views.py)
        bool cubeInject = false;           // CRYMT_GI_CUBEINJECT=1: cells from the engine cube's faces (kVoxShader "inject": the
                                           // surfaces each face's depth shows, with its lit colour, outside the view). Off: on
                                           // mine (2026-11-13, noon) it added only 3-5k level-0 cells, the cells found on
                                           // turning were 93-98% surfaces the cube cannot see (backs, hidden parts), and
                                           // inside the light change on turning grew (> 48/255: 14% of cells vs 1.4%)
        float lampLight = 1.0f;            // CRYMT_GI_LAMPS: the lamps' light on the geometry's cells (0 none)
        bool lampTest = false;             // CRYMT_GI_LAMPTEST=1: also on cells the screen coloured (to see it in the voxel view)
        int mipOpacity = 4;                // CRYMT_GI_MIPOPACITY: coarse cells' opacity, 0 the mean, 1/2/3 by rays through, 4 per axis (downsample)
        float ambientFloor = 0.1f;         // CRYMT_GI_FLOOR: the share of the flat ambient full occlusion keeps (Strengthen)
        float outdoorStrength = 0.7f;      // CRYMT_GI_OUTDOOR: the occlusion strength outdoors (Num4 / Num7 set the indoor one);
                                           // 1.0 before 2026-11-14 (with the voxels' occlusion 1.2 the cones darkened open
                                           // yards x0.15-0.2 against no GI; the indoor strength keeps the rooms dark), then
                                           // 0.45 (forest lighter than no GI), 0.7 the user's pick (forest 44.7, no GI 49.9)
        bool roofCells = false;            // CRYMT_GI_ROOFCELLS=1: the roof texture (kVoxShader "roof") as well as the ray
        bool thinFix = true;               // CRYMT_GI_THIN=0: thin features keep their own (chance) GI
        bool roomLight = true;             // CRYMT_GI_ROOMLIGHT=0: cones from inside a VisArea take light from outside it too
                                           // (2026-11-13: sunlit roofs and outer walls leaked in through the coarse mips)
        int dirLight = 1;                  // CRYMT_GI_DIRLIGHT: the cells' light, 0 one colour for all directions, 1 a colour
                                           // and the direction it goes out stronger (L1: a thin wall lit outside stays dark
                                           // inside; the voxels page; default since 2026-11-14, the user), 2 six directions
                                           // (+-X, +-Y, +-Z, ~220 MB more, made when first asked for)
        int probes = 0;                    // CRYMT_GI_PROBES=1: GI v2, irradiance probes traced through the voxels in place
                                           // of the cones near the camera (kGiShader "probes", ProbeLight); 2: the probes'
                                           // light with the cones' occlusion
        bool probeVisibility = true;       // CRYMT_GI_PROBEVIS=0: no visibility test (measures the light it keeps out)
        float probeStrength = 1.5f;        // CRYMT_GI_PROBESTRENGTH: their bounce light's factor (the cones' is voxelStrength
                                           // 1.5: coarse mips lose light; the probes' thin rays see the cells' full light).
                                           // 1.0 until 2026-11-14 (at 1.5 the loop through the screen's colour once lit
                                           // closed rooms by itself, before the probes' fixes); 1.5: the user's +50%
        float gtaoRadius = 0.5f;          // CRYMT_GI_GTAORADIUS, metres
        bool contactShadows = true;        // CRYMT_CONTACTSHADOWS=0: no screen-space contact shadows in the sun's shadow mask
        float contactLength = 0.35f;       // CRYMT_CONTACTLENGTH: the march's length at 10 m (metres; scales with distance)
        bool contactFoliage = true;        // CRYMT_CONTACTFOLIAGE=0: foliage (the z-prepass mask) casts no contact shadows (default on since 2026-11-10, the user)
        bool sss = true;                   // CRYMT_SSS=0: the old contact shadows (12 noisy steps in the shadow mask) in place of
                                           // Bend Studio's screen-space shadows (RunSss, BendSss.h)
        int sssSamples = 60;               // CRYMT_SSS_SAMPLES: their length in screen pixels (Bend: 60; compiled in)
        float sssThickness = 0.005f;       // CRYMT_SSS_THICKNESS: a pixel's assumed thickness, a share of its depth (Bend: 0.005)
        float sssContrast = 4.0f;          // CRYMT_SSS_CONTRAST (Bend: 2..4)
        float sssEdge = 0.02f;             // CRYMT_SSS_EDGE: a depth step taken as an edge, a share of the depth (Bend: 0.02)
        float sssReach = 4000.0f;          // CRYMT_SSS_REACH: metres from the camera; faded over the last quarter (the
                                           // materials take them beyond the shadow maps too: fragLib, Terrain). 4000 m
                                           // since 2026-11-12 (the user; 0.235 ms at 5120x1440 against 0.231 at 1000 m)
        float sssNear = 0.0f;              // CRYMT_SSS_NEAR: metres; nearer pixels get none
        float sssWeaponThickness = 0.1f;   // CRYMT_SSS_WEAPONTHICK: the first-person view's thickness, a share of its depth (at
                                           // the world's 0.005 the weapon cast no shadow on the hands)
        float gtao = 1.5f;                 // CRYMT_GI_GTAO=<strength>: GTAO in place of the engine's SSAO, a factor on the GI's
                                           // result as the SSAO was (village rooms: darkening 41.8/68.7/48.2% at 1.5 against the
                                           // SSAO's 44.1/69.8/54.2%; creases darker than the SSAO, open walls not darkened)
        bool replaceSsao = true;           // CRYMT_GI_SSAO=1: keep the engine's SSAO (on top of the GI's occlusion: it doubled
                                           // it; without GTAO the creases came out light, the user saw light in the corners)
        int bakedOff = 30;                 // the baked occlusion the GI replaces near the camera, bits 2 the objects' RAM, 4 their
                                           // sky amount, 8 TerrainAO (under trees), 16 the terrain's vertex AO: by default all
                                           // (the user: our GI near, the baked further, in full far away); CRYMT_GI_BAKED=engine
                                           // none (the engine's everywhere, under the GI), or a list (ram,sky,terrainao,vertex)
        float terrainAoNear = 62.0f;       // CRYMT_GI_TERRAINAONEAR: metres up to which the GI alone works; over 1.5 x that
                                           // the GI fades out and the baked terms in (far trees are sprites, beyond the voxels);
                                           // 62 (the panel's top, "up to 155") since 2026-11-14, the user (30 before)
        bool gtaoFull = false;             // CRYMT_GI_GTAOFULL=1: GTAO at full size (island 5120x1440: 3.5 ms against 1.2)
        bool gtaoNoFoliage = false;        // CRYMT_GI_GTAOFOLIAGE=0: grass and leaves (the foliage mask) occlude nothing in GTAO
                                           // (the user: plants looked worse without their occlusion)
        float foliageBounce = 0.5f;        // CRYMT_GI_FOLIAGEBOUNCE: the bounce light on leaves times this (their occlusion
                                           // keeps CRYMT_GI_FOLIAGEGI); 0.5 with the bounce +50% overall = half of before
                                           // (the user, 2026-11-14)
        float grassGi = 1.0f;              // CRYMT_GI_GRASSGI: the same for grass (marked apart by the z-prepass), which keeps
                                           // the engine's SSAO in place of GTAO (2026-11-10, the user: grass flat, too bright)
        float foliageGi = 1.5f;            // CRYMT_GI_FOLIAGEGI: the GI's occlusion and bounce on grass and leaves (the foliage
                                           // mask) times this (the user: more of the voxel GI on grass); 1 as everything else
        bool particlesGi = true;           // CRYMT_GI_PARTICLES=0 (key "-"): blended particles (smoke, dust) without the GI
        int vfog = 1;                      // CRYMT_VFOG: the GI in the engine's fog (RunVfog, the patched FogPass): 0 off, 1
                                           // on, 2 the volume made but zero (must give the engine's fog exactly: a check)
        float vfogGi = 1.0f;               // CRYMT_VFOG_GI: its strength (the particles' ambient share lost, times this)
        float vfogSun = 1.0f;              // CRYMT_VFOG_SUN: the sun's shadow maps in the fog (stage 2: its haze dark where the
                                           // cascades see shade, outdoors too); 0 off (stage 1: the voxels' sun indoors)
        float vfogHaze = 0.0f;             // CRYMT_VFOG_HAZE: the near air - the share of light a haze of its own takes over
                                           // its reach (0 none; the engine's fog near the camera is thin, the sun's shafts
                                           // hardly show without it; the user's panel row "Плотность воздуха вблизи")
        float vfogHazeReach = 60.0f;       // CRYMT_VFOG_HAZEREACH: its reach (m): full density to half of it, none at it
        int vfogSamples = 8;               // CRYMT_VFOG_SAMPLES: the sun's shadow samples per cell, 1 / 4 / 8 (one drew the
                                           // shadows' edges in the fog as steps of whole tiles: the user, Sponza 2026-11-15;
                                           // 8 by default, the user)
        float vfogOutdoor = 1.0f;          // CRYMT_VFOG_OUT: its darkening outdoors against indoors (the fog's own; the
                                           // particles' 200% made the path under palms too dark, the user 2026-11-14)
        float particleCell = 1.5f;         // CRYMT_GI_PARTICLECELL: their coarse occlusion volume's cell (m; 64 x 64 x 32 cells)
        float particleFineCell = 0.5f;     // CRYMT_GI_PARTICLEFINE: the fine one's (rooms' walls and ceilings)
        float particleOutdoor = 2.0f;      // CRYMT_GI_PARTICLEOUT: the darkening outdoors against indoors (key "=" 50 / 70 / 100 /
                                           // 150 / 200%; the user wanted more than 100%; thin particles keep more of their light)
        float particleMax = 0.85f;         // CRYMT_GI_PARTICLEMAX: at most this share of a particle's ambient goes
        float particleTint = 0.5f;         // CRYMT_GI_PARTICLETINT: their ambient takes this much of the voxels' light hue
        bool gtaoFoliageShade = true;      // CRYMT_GI_GTAOFOLIAGESHADE=0: GTAO at full strength also on foliage in the sun's
                                           // shadow (the user: there it was too strong; in sunlight fine): 0.2 there
        bool gtaoHistory = false;          // CRYMT_GI_GTAOHISTORY=1: its own history (0.9, reprojected): it swam on camera
                                           // turns; without, the slice turns move per frame and DLAA accumulates them
        float gtaoHistoryWeight = 0.9f;    // ... the history's weight: CRYMT_GI_GTAOHISTORY=<under 1> or the AO page's
                                           // "Накопление GTAO" (the user, 2026-11-14: GTAO noisy on small objects and
                                           // leaves while moving; 0.9 swam)
    };
    static const GiSettings& GiBase() // the ini / environment values
    {
        static const GiSettings s = [] {
            GiSettings v;
            wchar_t text[32] = {};
            v.on = GetEnvironmentVariableW(L"CRYMT_GI", text, 32) && wcscmp(text, L"0") != 0;
            const auto read = [](const wchar_t* name, float& value) {
                wchar_t t[32] = {};
                if (GetEnvironmentVariableW(name, t, 32))
                    value = static_cast<float>(_wtof(t));
            };
            read(L"CRYMT_GI_TEMPORAL", v.temporal);
            read(L"CRYMT_GI_DISTANCE", v.maxDistance);
            read(L"CRYMT_GI_DEBUG", v.debug);
            v.upsample = !(GetEnvironmentVariableW(L"CRYMT_GI_UPSAMPLE", text, 32) && wcscmp(text, L"0") == 0);
            v.fixedNoise = GetEnvironmentVariableW(L"CRYMT_GI_FIXEDNOISE", text, 32) && wcscmp(text, L"0") != 0;
            v.nearest = GetEnvironmentVariableW(L"CRYMT_GI_NEAREST", text, 32) && wcscmp(text, L"0") != 0;
            v.narrowNormals = GetEnvironmentVariableW(L"CRYMT_GI_NARROWNORMALS", text, 32) && wcscmp(text, L"0") != 0;
            v.weaponGi = !(GetEnvironmentVariableW(L"CRYMT_GI_WEAPON", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_GI_OCCRANGE", v.occFar);
            read(L"CRYMT_GI_OCCSTRENGTH", v.occStrength);
            // The voxel cones are the GI; CRYMT_GI_VOXELS=0: no grid (no GI, to measure), 4 the grid without cones.
            const bool voxelVar = GetEnvironmentVariableW(L"CRYMT_GI_VOXELS", text, 32) != 0;
            v.voxels = voxelVar ? wcscmp(text, L"0") != 0 : v.on;
            v.voxelMix = !v.voxels ? 0.0f : voxelVar && _wtof(text) == 4 ? 4.0f : 1.0f;
            read(L"CRYMT_GI_VOXELSTRENGTH", v.voxelStrength);
            read(L"CRYMT_GI_VOXELREACH", v.voxelReach);
            read(L"CRYMT_GI_VOXELAO", v.voxelOcclusion);
            read(L"CRYMT_GI_VOXELSIZE", v.voxelSize);
            read(L"CRYMT_GI_VOXELVIEW", v.voxelView);
            v.voxels = v.voxels || v.voxelView > 0;
            const bool geometryVar = GetEnvironmentVariableW(L"CRYMT_GI_GEOMETRY", text, 32) != 0;
            v.geometry = v.voxels && !(geometryVar && wcscmp(text, L"0") == 0);
            v.geometryFoliage = v.geometry && geometryVar && wcscmp(text, L"2") == 0;
            v.geometryPersist = !(GetEnvironmentVariableW(L"CRYMT_GI_GEOMPERSIST", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_GI_FOLIAGE", v.foliageOpacity);
            v.opacityGeometry = !(GetEnvironmentVariableW(L"CRYMT_GI_OCCUPANCY", text, 32) && _wcsicmp(text, L"screen") == 0);
            v.materials = !(GetEnvironmentVariableW(L"CRYMT_GI_MATERIALS", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_GI_CONELIFT", v.coneLift);
            read(L"CRYMT_GI_CONESTART", v.coneStart);
            read(L"CRYMT_GI_NEAROCC", v.nearOcclusion);
            read(L"CRYMT_GI_SUNALBEDO", v.sunAlbedo);
            read(L"CRYMT_GI_SKYFILL", v.skyFill);
            {
                float steps = static_cast<float>(v.sunMarch);
                read(L"CRYMT_GI_SUNMARCH", steps);
                v.sunMarch = std::min(std::max(static_cast<int>(steps), 1), 128);
            }
            v.shadowGeometry = GetEnvironmentVariableW(L"CRYMT_GI_SHADOWGEOM", text, 32) && wcscmp(text, L"0") != 0;
            v.cubeGeometry = GetEnvironmentVariableW(L"CRYMT_GI_CUBEGEOM", text, 32) && wcscmp(text, L"0") != 0;
            v.cubeInject = GetEnvironmentVariableW(L"CRYMT_GI_CUBEINJECT", text, 32) && wcscmp(text, L"0") != 0;
            read(L"CRYMT_GI_LAMPS", v.lampLight);
            v.lampTest = GetEnvironmentVariableW(L"CRYMT_GI_LAMPTEST", text, 32) && wcscmp(text, L"0") != 0;
            if (GetEnvironmentVariableW(L"CRYMT_GI_MIPOPACITY", text, 32))
                v.mipOpacity = _wtoi(text);
            read(L"CRYMT_GI_FLOOR", v.ambientFloor);
            read(L"CRYMT_GI_OUTDOOR", v.outdoorStrength);
            v.roofCells = GetEnvironmentVariableW(L"CRYMT_GI_ROOFCELLS", text, 32) && wcscmp(text, L"0") != 0;
            v.thinFix = !(GetEnvironmentVariableW(L"CRYMT_GI_THIN", text, 32) && wcscmp(text, L"0") == 0);
            v.roomLight = !(GetEnvironmentVariableW(L"CRYMT_GI_ROOMLIGHT", text, 32) && wcscmp(text, L"0") == 0);
            if (GetEnvironmentVariableW(L"CRYMT_GI_DIRLIGHT", text, 32))
                v.dirLight = std::min(std::max(_wtoi(text), 0), 2);
            if (GetEnvironmentVariableW(L"CRYMT_GI_PROBES", text, 32))
                v.probes = std::min(std::max(_wtoi(text), 0), 2);
            read(L"CRYMT_GI_PROBESTRENGTH", v.probeStrength);
            v.probeVisibility = !(GetEnvironmentVariableW(L"CRYMT_GI_PROBEVIS", text, 32) && wcscmp(text, L"0") == 0);
            v.replaceSsao = !(GetEnvironmentVariableW(L"CRYMT_GI_SSAO", text, 32) && wcscmp(text, L"0") != 0);
            read(L"CRYMT_GI_GTAO", v.gtao);
            read(L"CRYMT_GI_GTAORADIUS", v.gtaoRadius);
            v.contactShadows = !(GetEnvironmentVariableW(L"CRYMT_CONTACTSHADOWS", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_CONTACTLENGTH", v.contactLength);
            v.contactFoliage = !(GetEnvironmentVariableW(L"CRYMT_CONTACTFOLIAGE", text, 32) && wcscmp(text, L"0") == 0);
            v.sss = !(GetEnvironmentVariableW(L"CRYMT_SSS", text, 32) && wcscmp(text, L"0") == 0);
            if (GetEnvironmentVariableW(L"CRYMT_SSS_SAMPLES", text, 32))
                v.sssSamples = std::min(std::max(_wtoi(text), 16), 256);
            read(L"CRYMT_SSS_THICKNESS", v.sssThickness);
            read(L"CRYMT_SSS_CONTRAST", v.sssContrast);
            read(L"CRYMT_SSS_EDGE", v.sssEdge);
            read(L"CRYMT_SSS_REACH", v.sssReach);
            read(L"CRYMT_SSS_NEAR", v.sssNear);
            read(L"CRYMT_SSS_WEAPONTHICK", v.sssWeaponThickness);
            if (GetEnvironmentVariableW(L"CRYMT_GI_BAKED", text, 32))
            {
                _wcslwr_s(text);
                v.bakedOff = wcscmp(text, L"0") == 0 ? 30 : wcscmp(text, L"engine") == 0 ? 0
                                                     : (wcsstr(text, L"ram") ? 2 : 0) | (wcsstr(text, L"sky") ? 4 : 0) | (wcsstr(text, L"terrainao") ? 8 : 0) |
                                                           (wcsstr(text, L"vertex") ? 16 : 0);
            }
            read(L"CRYMT_GI_TERRAINAONEAR", v.terrainAoNear);
            v.gtaoFull = GetEnvironmentVariableW(L"CRYMT_GI_GTAOFULL", text, 32) && wcscmp(text, L"0") != 0;
            v.gtaoHistory = GetEnvironmentVariableW(L"CRYMT_GI_GTAOHISTORY", text, 32) && wcscmp(text, L"0") != 0;
            if (v.gtaoHistory)
            {
                const float w = static_cast<float>(_wtof(text));
                v.gtaoHistoryWeight = w > 0.0f && w < 1.0f ? w : 0.9f; // "1": the old 0.9
            }
            v.gtaoNoFoliage = GetEnvironmentVariableW(L"CRYMT_GI_GTAOFOLIAGE", text, 32) && wcscmp(text, L"0") == 0;
            read(L"CRYMT_GI_FOLIAGEGI", v.foliageGi);
            read(L"CRYMT_GI_FOLIAGEBOUNCE", v.foliageBounce);
            read(L"CRYMT_GI_GRASSGI", v.grassGi);
            v.particlesGi = !(GetEnvironmentVariableW(L"CRYMT_GI_PARTICLES", text, 32) && wcscmp(text, L"0") == 0);
            if (GetEnvironmentVariableW(L"CRYMT_VFOG", text, 32))
                v.vfog = std::min(std::max(_wtoi(text), 0), 2);
            read(L"CRYMT_VFOG_GI", v.vfogGi);
            v.vfogGi = std::min(std::max(v.vfogGi, 0.0f), 3.0f);
            read(L"CRYMT_VFOG_SUN", v.vfogSun);
            v.vfogSun = std::min(std::max(v.vfogSun, 0.0f), 1.0f);
            read(L"CRYMT_VFOG_HAZE", v.vfogHaze);
            v.vfogHaze = std::min(std::max(v.vfogHaze, 0.0f), 0.9f);
            if (GetEnvironmentVariableW(L"CRYMT_VFOG_SAMPLES", text, 32))
                v.vfogSamples = _wtoi(text) >= 8 ? 8 : _wtoi(text) >= 4 ? 4 : 1;
            read(L"CRYMT_VFOG_HAZEREACH", v.vfogHazeReach);
            v.vfogHazeReach = std::min(std::max(v.vfogHazeReach, 5.0f), 128.0f);
            read(L"CRYMT_VFOG_OUT", v.vfogOutdoor);
            v.vfogOutdoor = std::min(std::max(v.vfogOutdoor, 0.0f), 3.0f);
            read(L"CRYMT_GI_PARTICLECELL", v.particleCell);
            v.particleCell = v.particleCell > 0.25f ? v.particleCell : 1.5f;
            read(L"CRYMT_GI_PARTICLEFINE", v.particleFineCell);
            v.particleFineCell = v.particleFineCell > 0.1f ? v.particleFineCell : 0.5f;
            read(L"CRYMT_GI_PARTICLEOUT", v.particleOutdoor);
            read(L"CRYMT_GI_PARTICLEMAX", v.particleMax);
            read(L"CRYMT_GI_PARTICLETINT", v.particleTint);
            v.gtaoFoliageShade = !(GetEnvironmentVariableW(L"CRYMT_GI_GTAOFOLIAGESHADE", text, 32) && wcscmp(text, L"0") == 0);
            v.voxelSize = v.voxelSize > 0.05f ? v.voxelSize : 0.25f;
            return v;
        }();
        return s;
    }

    // The live values: the ini's, with the GI test keys' switches applied at present (stream thread, as every reader).
    static GiSettings& Gi()
    {
        static GiSettings live = GiBase();
        return live;
    }

    // The contact shadows' mode as the ini says: 0 off, 1 the old ones, 2 Bend's screen-space shadows.
    static int BaseContact()
    {
        const GiSettings& b = GiBase();
        return b.contactShadows ? (b.sss ? 2 : 1) : 0;
    }

    void GiApplyControl()
    {
        static const bool fixCopiesEnv = [] { // CRYMT_DLSS_FIXCOPIES=0: off from the start (key "]")
            wchar_t text[8] = {};
            if (GetEnvironmentVariableW(L"CRYMT_DLSS_FIXCOPIES", text, 8) && wcscmp(text, L"0") == 0)
                g_giControl.fixCopies = 0;
            return true;
        }();
        (void)fixCopiesEnv;
        if (!g_giControl.active)
            return;
        const GiSettings& b = GiBase();
        GiSettings& g = Gi();
        const float occlusion = g_giControl.occlusionScale, bounce = g_giControl.bounceScale;
        const bool wasOn = g.on;
        g.on = b.on && g_giControl.enabled;
        if (g.on && !wasOn)
            m_giReset = true; // the history is from before
        if (!g.on && !m_giSrv)
            m_giCpu = {}; // shaders see the placeholder: no GI (CRYMT_GITEST keeps its texture)
        g.occStrength = b.occStrength * occlusion; // Num4 / Num7: where there is cover above (Strengthen), not everywhere
        g.voxelStrength = b.voxelStrength * bounce;
        g.probeStrength = b.probeStrength * bounce; // GI v2 too (the user liked Sponza's strong bounce)
        g.voxels = b.on; // the grid keeps up, so the cones and the view switch on at once
        g.voxelMix = b.voxelMix == 4 ? 4.0f : 1.0f; // the cones (as the grid, they keep up with GI on)
        g.weaponGi = b.weaponGi && g_giControl.weapon;
        g.voxelView = static_cast<float>(g_giControl.view.load());
        // Num3: the voxel geometry with the near occlusion cones; off gives the ini's settings exactly.
        if (g_giControl.geometry < 0)
            g_giControl.geometry = b.geometry ? (b.geometryFoliage ? 2 : 1) : 0;
        const int geometry = g_giControl.geometry;
        g.geometry = b.voxels && geometry > 0;
        g.geometryFoliage = geometry == 2;
        g.nearOcclusion = geometry > 0 ? (b.nearOcclusion > 0 ? b.nearOcclusion : 1.0f) : 0.0f;
        // Voxels page Num0: where the cells' opacity comes from.
        if (g_giControl.voxOpacity < 0)
            g_giControl.voxOpacity = b.opacityGeometry ? 1 : 0;
        g.opacityGeometry = g_giControl.voxOpacity == 1 && g.geometry;
        // Voxels page Num1: unchanged objects every 4th frame or every frame.
        // PgDn: GTAO in place of the engine's SSAO, or the SSAO; Home: contact shadows.
        if (g_giControl.gtao < 0)
            g_giControl.gtao = b.replaceSsao ? 1 : 0;
        g.replaceSsao = g_giControl.gtao > 0;
        if (g_giControl.contact < 0)
            g_giControl.contact = BaseContact();
        g.contactShadows = g_giControl.contact > 0;
        g.sss = g_giControl.contact == 2;
        g.sssReach = g_giControl.sssReach > 0 ? static_cast<float>(g_giControl.sssReach) : b.sssReach;
        g.sssWeaponThickness = g_giControl.sssWeapon >= 0 && g_giControl.sssWeapon < 5 ? kSssWeaponThick[g_giControl.sssWeapon] : b.sssWeaponThickness;
        g.terrainAoNear = g_giControl.giNear >= 0 && g_giControl.giNear < 4 ? static_cast<float>(kGiNears[g_giControl.giNear]) : b.terrainAoNear;
        g.dirLight = g_giControl.dirLight >= 0 ? std::min(g_giControl.dirLight.load(), 2) : b.dirLight;
        g.probes = g_giControl.probes >= 0 ? std::min(g_giControl.probes.load(), 2) : b.probes;
        if (g_giControl.gtaoHistory >= 0) // the AO page's "Накопление GTAO": off, 0.5, 0.7, 0.9
        {
            static constexpr float kWeights[4] = {0.0f, 0.5f, 0.7f, 0.9f};
            const int i = std::min(g_giControl.gtaoHistory.load(), 3);
            g.gtaoHistory = i > 0;
            g.gtaoHistoryWeight = i > 0 ? kWeights[i] : b.gtaoHistoryWeight;
        }
        if (g_giControl.particles < 0)
            g_giControl.particles = b.particlesGi ? 1 : 0;
        g.particlesGi = g_giControl.particles > 0;
        if (g_giControl.vfog < 0)
            g_giControl.vfog = b.vfog;
        g.vfog = std::min(g_giControl.vfog.load(), 2);
        g.vfogGi = g_giControl.vfogGi >= 0 ? g_giControl.vfogGi / 100.0f : b.vfogGi;
        g.vfogOutdoor = g_giControl.vfogOut >= 0 ? g_giControl.vfogOut / 100.0f : b.vfogOutdoor;
        g.vfogSun = g_giControl.vfogSun >= 0 ? std::min(g_giControl.vfogSun / 100.0f, 1.0f) : b.vfogSun;
        g.vfogHaze = g_giControl.vfogHaze >= 0 ? std::min(g_giControl.vfogHaze / 100.0f, 0.9f) : b.vfogHaze;
        g.vfogSamples = g_giControl.vfogSamples >= 0 ? (g_giControl.vfogSamples >= 8 ? 8 : g_giControl.vfogSamples >= 4 ? 4 : 1) : b.vfogSamples;
        if (g_giControl.particleOut < 0)
            g_giControl.particleOut = static_cast<int>(b.particleOutdoor * 100.0f + 0.5f);
        g.particleOutdoor = g_giControl.particleOut / 100.0f;
        if (g_giControl.bakedOff < 0)
            g_giControl.bakedOff = b.bakedOff;
        g.bakedOff = g_giControl.bakedOff & 30;
    }

    // A null state is D3D10's default rasterizer: solid. The editor's wireframe views share the main renderer and the
    // scene's targets; they must not become the GI's source nor run its pass (GetMainViewport.patch, 2026-11-14).
    bool GiSolidFill() const { return !m_raster || m_raster->desc.FillMode == D3D10_FILL_SOLID; }

    void GiSceneDone(const DlssSceneInfo* info)
    {
        if (!Gi().on || !GiSolidFill() || !info || !info->valid || !m_rtv[0] || !m_dsv)
            return;
        auto* color = static_cast<TextureCore*>(m_rtv[0]->info);
        auto* depth = static_cast<TextureCore*>(m_dsv->info);
        if (depth->width < color->width || depth->height < color->height || color->samples != 1 || depth->samples != 1 ||
            !EnsureGi(color->width, color->height, color->format))
            return;
        m_giSceneDepth = m_dsv->info;
        memcpy(m_giSun, info->sun, sizeof(m_giSun));
        memcpy(m_giLamps, info->lamps, sizeof(m_giLamps));
        m_giLampCount = std::min<uint32_t>(info->lampCount, 8);
        memcpy(m_giVisAreas, info->visAreas, sizeof(m_giVisAreas));
        m_giVisAreaCount = std::min<uint32_t>(info->visAreaCount, 16);
        if (m_rtv[0]->info != m_giSceneColor)
        {
            m_giSceneColorPrev = m_giSceneColor;
            m_giSceneColor = m_rtv[0]->info;
        }
        Require(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_barriers.push_back(Transition(m_giColor, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
        FlushBarriers();
        m_list->CopyResource(m_giColor, color->res);
        m_barriers.push_back(Transition(m_giColor, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
        m_giColorFrame = m_frame;
        m_giFrame++;
    }

    // The frame's main camera from the stream (kOpCamera): the scene's draws follow, the GI pass may run.
    // The frame texture (Textures/CryMT/jitter.dds's stand-in, 8x1 RGBA32F) for the patched ShadowMaskGen.cfx:
    // 0 the camera jitter (shiftU, shiftV) and the frame (0..63, 0 without jitter): the shader rebuilds a pixel's point
    // at the pixel minus the jitter, where the jittered depth's point lies; with the engine's unjittered frustum rays
    // the point left the surface by the depth gradient times up to half a pixel, and flat surfaces at grazing light
    // flipped in and out of shadow with each jitter phase (village shelves: 4366 changed pixels a frame, 6 without
    // shadows, 9 without DLAA). 1 towards the sun, w 2 with contact shadows; 2..5 the columns of the unjittered
    // view-projection (row vectors); 6 the contact shadows' length and thickness at 10 m, their reach (metres).
    // Set with the frame's main camera, before its draws.
    // The patched z-prepass (SV_Target1: 1 on alpha-tested surfaces) into the scene's R32F depth target: the foliage
    // mask bound as the second target (made at that target's size), cleared at the frame's first such draw. Targets
    // smaller than the previous frame's largest z-prepass (the reflections') are left out. That was "the largest ever"
    // until 2026-11-13: after the game's resolution went down (5120 -> 2560 wide) the scene's own z-prepass counted as a
    // reflection, so no mask, no depth for the screen-space shadows and the water's SSR for the rest of the session.
    // Wind-bent vegetation's screen move (R16G16F, the scene's size): the patched z-prepass's SV_Target2 (before minus now,
    // screen fractions per 1/60 s; 0 on everything not bent), cleared at the frame's first z-prepass draw. RunMotion
    // adds it scaled to the frame's time.
    void BindBendVelocity(UINT w, UINT h)
    {
        if (!m_bendRtv || w != m_bendW || h != m_bendH)
        {
            if (m_bendSrv)
                m_bendSrv->Release();
            if (m_bendRtv)
                m_bendRtv->Release();
            if (m_bendTex)
                m_bendTex->Release();
            m_bendSrv = nullptr;
            m_bendRtv = nullptr;
            m_bendTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16G16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("bent vegetation's motion %ux%u: failed", w, h);
                return;
            }
            m_bendTex = tex;
            m_bendRtv = static_cast<Rtv12*>(rtv);
            m_bendSrv = static_cast<Srv12*>(srv);
            m_bendW = w;
            m_bendH = h;
            m_bendFrame = ~0ull;
            Log("bent vegetation's motion %ux%u", w, h);
        }
        if (m_bendFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_bendRtv, kZero);
            m_bendFrame = m_frame;
        }
        m_bendRtv->info->outputBound++;
        Bind(m_rtv[2], m_bendRtv);
    }

    // The particle layer for the water's SSR (R16F, the scene's size; two, a frame each, so the water reads the last frame's
    // while this frame's is drawn): the patched particles' SV_Target2, the nearest kept (Pso: MIN), cleared to far (65504)
    // at the frame's first particle draw. CRYMT_SSR_PARTICLES=0: none.
    void BindParticleLayer()
    {
        static const bool on = [] {
            wchar_t v[8] = {};
            return !(GetEnvironmentVariableW(L"CRYMT_SSR_PARTICLES", v, 8) && wcscmp(v, L"0") == 0);
        }();
        if (!on || !WaterSsrOn())
            return;
        const UINT w = m_foliageW, h = m_foliageH;
        if (!m_partRtv[0] || w != m_partW || h != m_partH)
        {
            for (int i = 0; i < 2; i++)
            {
                if (m_partSrv[i])
                    m_partSrv[i]->Release();
                if (m_partRtv[i])
                    m_partRtv[i]->Release();
                if (m_partTex[i])
                    m_partTex[i]->Release();
                m_partSrv[i] = nullptr;
                m_partRtv[i] = nullptr;
                m_partTex[i] = nullptr;
                m_partFrame[i] = ~0ull;
            }
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            for (int i = 0; i < 2; i++)
            {
                ID3D10Texture2D* tex = nullptr;
                ID3D10RenderTargetView* rtv = nullptr;
                ID3D10ShaderResourceView* srv = nullptr;
                if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                    FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
                {
                    if (rtv)
                        rtv->Release();
                    if (tex)
                        tex->Release();
                    Log("particle layer %ux%u: failed", w, h);
                    return;
                }
                m_partTex[i] = tex;
                m_partRtv[i] = static_cast<Rtv12*>(rtv);
                m_partSrv[i] = static_cast<Srv12*>(srv);
            }
            m_partW = w;
            m_partH = h;
            Log("particle layer %ux%u (the water's SSR)", w, h);
        }
        const int k = static_cast<int>(m_frame & 1);
        if (m_partFrame[k] != m_frame)
        {
            static const FLOAT kFar[4] = {65504.0f, 65504.0f, 65504.0f, 65504.0f};
            ClearRenderTargetView(m_partRtv[k], kFar);
            m_partFrame[k] = m_frame;
        }
        m_partDraws++;
        m_partRtv[k]->info->outputBound++;
        Bind(m_rtv[2], m_partRtv[k]);
        m_omDirty = m_psoDirty = true;
    }

    // The last frame's particle layer, when it was drawn (the water's SSR).
    Srv12* LastParticleLayer() const
    {
        const int k = static_cast<int>((m_frame + 1) & 1);
        return m_frame > 0 && m_partSrv[k] && m_partFrame[k] == m_frame - 1 ? m_partSrv[k] : nullptr;
    }

    // DLSS's bias towards the current colour (R16F, the scene's size): the patched water and particle pixel shaders'
    // SV_Target1, blended like the colour (Pso: red only), cleared at the frame's first such draw.
    void BindBiasMask()
    {
        const UINT w = m_foliageW, h = m_foliageH;
        if (!m_biasRtv || w != m_biasW || h != m_biasH)
        {
            if (m_biasSrv)
                m_biasSrv->Release();
            if (m_biasRtv)
                m_biasRtv->Release();
            if (m_biasTex)
                m_biasTex->Release();
            m_biasSrv = nullptr;
            m_biasRtv = nullptr;
            m_biasTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("DLSS bias mask %ux%u: failed", w, h);
                return;
            }
            m_biasTex = tex;
            m_biasRtv = static_cast<Rtv12*>(rtv);
            m_biasSrv = static_cast<Srv12*>(srv);
            m_biasW = w;
            m_biasH = h;
            m_biasFrame = ~0ull;
            Log("DLSS bias mask %ux%u", w, h);
        }
        if (m_biasFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_biasRtv, kZero);
            m_biasFrame = m_frame;
            m_stats.biasFrames++;
        }
        m_stats.biasDraws++;
        m_biasRtv->info->outputBound++;
        Bind(m_rtv[1], m_biasRtv);
        m_omDirty = m_psoDirty = true;
    }

    // Exact object motion (R16G16B16A16F, the motion blur target's size): the patched motion blur pass's SV_Target1
    // (Settings.cpp kMotionBlurExact: before minus now in screen fractions, distance / far, 1 exact or 0.5 with the
    // engine's shutter factor still in; 0 where no moving object was drawn), cleared at the frame's first such draw.
    // RunMotion reads it in place of the engine's own target.
    void BindObjectMotion()
    {
        const auto* t = static_cast<const TextureCore*>(m_rtv[0]->info);
        const UINT w = t->width, h = t->height;
        if (!m_objMvRtv || w != m_objMvW || h != m_objMvH)
        {
            if (m_objMvSrv)
                m_objMvSrv->Release();
            if (m_objMvRtv)
                m_objMvRtv->Release();
            if (m_objMvTex)
                m_objMvTex->Release();
            m_objMvSrv = nullptr;
            m_objMvRtv = nullptr;
            m_objMvTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("exact object motion %ux%u: failed", w, h);
                return;
            }
            m_objMvTex = tex;
            m_objMvRtv = static_cast<Rtv12*>(rtv);
            m_objMvSrv = static_cast<Srv12*>(srv);
            m_objMvW = w;
            m_objMvH = h;
            m_objMvFrame = ~0ull;
            Log("exact object motion %ux%u", w, h);
        }
        if (m_objMvFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_objMvRtv, kZero);
            m_objMvFrame = m_frame;
            m_stats.objectMotionExactFrames++;
        }
        m_stats.objectMotionExactDraws++;
        m_objMvRtv->info->outputBound++;
        Bind(m_rtv[1], m_objMvRtv);
        m_omDirty = m_psoDirty = true;
    }

    void BindFoliageMask()
    {
        const auto* t = static_cast<const TextureCore*>(m_rtv[0]->info);
        const UINT w = t->width, h = t->height;
        if (m_zMaxFrame != m_frame)
        {
            if (m_zMaxW)
            {
                m_zPrevW = m_zMaxW;
                m_zPrevH = m_zMaxH;
            }
            m_zMaxW = m_zMaxH = 0;
            m_zMaxFrame = m_frame;
        }
        m_zMaxW = std::max(m_zMaxW, w);
        m_zMaxH = std::max(m_zMaxH, h);
        if (w < m_zPrevW || h < m_zPrevH)
            return;
        if (!m_foliageRtv || w != m_foliageW || h != m_foliageH)
        {
            if (m_foliageSrv)
                m_foliageSrv->Release();
            if (m_foliageRtv)
                m_foliageRtv->Release();
            if (m_foliageTex)
                m_foliageTex->Release();
            m_foliageSrv = nullptr;
            m_foliageRtv = nullptr;
            m_foliageTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("foliage mask %ux%u: failed", w, h);
                return;
            }
            m_foliageTex = tex;
            m_foliageRtv = static_cast<Rtv12*>(rtv);
            m_foliageSrv = static_cast<Srv12*>(srv);
            m_foliageW = w;
            m_foliageH = h;
            m_foliageFrame = ~0ull;
            Log("foliage mask %ux%u", w, h);
        }
        if (m_foliageFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_foliageRtv, kZero);
            m_foliageFrame = m_frame;
            m_stats.foliageFrames++;
        }
        m_foliageRtv->info->outputBound++;
        Bind(m_rtv[1], m_foliageRtv);
        if (m_ps->writesTarget2 && !m_rtv[2])
            BindBendVelocity(w, h);
        m_omDirty = m_psoDirty = true;
        m_zTarget = static_cast<TextureCore*>(m_rtv[0]->info); // the depth for RunSss
        m_zTargetFrame = m_frame;
    }

    // The water's SSR reads the previous frame's finished scene (the GI's colour copy at SceneDone: after the particles and
    // clouds, which the scene copy Water.cfx reads is taken before; the user, 2026-11-15: "чтобы отражались particle и
    // облака"). Texels 8..10 (out[0..11]): this frame's homogeneous screen point s = (u w, v w, w, 1) - or a direction,
    // w 0 - to the previous frame's (u w, v w, w), each frame with its own jitter, in double; texel 11: 1 when the copy
    // is the last frame's, and its size. s = H A + t for H the point's offset from the camera (world), so H = (s - t)
    // A^-1; the previous view is (H + c - c') R', then its projection.
    void WaterPrevFrame(const DlssSceneInfo* info, float* out)
    {
        // The frame texture is updated at every main camera of a frame (6 at once seen on island): the frame's latest
        // camera is kept as this frame's, and moves to the previous one only when the frame changes (2026-11-15: each
        // later call took this frame's camera for the last one's and switched the previous frame off for the water).
        if (m_ssrCurFrame != m_frame)
        {
            if (m_ssrCurFrame != ~0ull)
            {
                memcpy(m_ssrPrevView, m_ssrCurView, sizeof(m_ssrPrevView));
                memcpy(m_ssrPrevProj, m_ssrCurProj, sizeof(m_ssrPrevProj));
                memcpy(m_ssrPrevCamPos, m_ssrCurCamPos, sizeof(m_ssrPrevCamPos));
                memcpy(m_ssrPrevShift, m_ssrCurShift, sizeof(m_ssrPrevShift));
                m_ssrPrevCamFrame = m_ssrCurFrame;
            }
            m_ssrCurFrame = m_frame;
        }
        memcpy(m_ssrCurView, info->view, sizeof(m_ssrCurView));
        memcpy(m_ssrCurProj, info->proj, sizeof(m_ssrCurProj));
        memcpy(m_ssrCurCamPos, info->camPos, sizeof(m_ssrCurCamPos));
        m_ssrCurShift[0] = info->shiftU;
        m_ssrCurShift[1] = info->shiftV;
        const bool prevCamera = m_ssrPrevCamFrame + 1 == m_frame;
        // CRYMT_SSR_PREV=0: the current scene copy only, as before (A/B).
        static const bool allowed = [] {
            wchar_t v[8] = {};
            return !(GetEnvironmentVariableW(L"CRYMT_SSR_PREV", v, 8) && wcscmp(v, L"0") == 0);
        }();
        m_ssrPrevOn = allowed && WaterSsrOn() && !m_hizFailed && m_giColor && m_giColorFrame + 1 == m_frame && prevCamera && !info->reset;
        static uint64_t logged, loggedFrame = ~0ull;
        if (logged < 3 && (m_frame % 600) == 599 && loggedFrame != m_frame)
        {
            logged++;
            loggedFrame = m_frame;
            Log("water SSR previous frame at frame %llu: on %d (allowed %d, SSR %d, pyramid failed %d, GI colour %d, its frame %llu, "
                "previous camera frame %llu, reset %u; particle layer last frame %d, draws %llu)",
                static_cast<unsigned long long>(m_frame), m_ssrPrevOn ? 1 : 0, allowed ? 1 : 0, WaterSsrOn() ? 1 : 0, m_hizFailed ? 1 : 0,
                m_giColor ? 1 : 0, static_cast<unsigned long long>(m_giColorFrame), static_cast<unsigned long long>(m_ssrPrevCamFrame),
                info->reset, LastParticleLayer() ? 1 : 0, static_cast<unsigned long long>(m_partDraws));
        }
        if (m_ssrPrevOn && m_ssrPrevRes != m_giColor)
        {
            if (!m_ssrPrevSrv.ptr)
                m_ssrPrevSrv = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_giColor, nullptr, m_ssrPrevSrv);
            m_ssrPrevRes = m_giColor;
        }
        if (m_ssrPrevOn)
        {
            // This frame: the columns of (u w, v w, w) over H (rows 0..2) and t (row 3).
            double a[4][3] = {};
            const double su = info->shiftU, sv = info->shiftV;
            for (int i = 0; i < 4; i++)
            {
                double x = 0, y = 0, w = 0;
                for (int j = 0; j < 3; j++)
                {
                    const double r = i < 3 ? static_cast<double>(info->view[i * 4 + j]) : 0.0;
                    x += r * info->proj[j * 4 + 0];
                    y += r * info->proj[j * 4 + 1];
                    w += r * info->proj[j * 4 + 3];
                }
                if (i == 3)
                {
                    x = info->proj[12];
                    y = info->proj[13];
                    w = info->proj[15];
                }
                a[i][0] = 0.5 * x + (0.5 + su) * w;
                a[i][1] = -0.5 * y + (0.5 + sv) * w;
                a[i][2] = w;
            }
            // A^-1 (3x3, row-vector convention: s = H A).
            const double (&m)[4][3] = a;
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            if (fabs(det) < 1e-12)
                m_ssrPrevOn = false;
            else
            {
                double inv[3][3];
                inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
                inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
                inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
                inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
                inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
                inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
                inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
                inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
                inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
                // H over s (rows 0..2) and the constant (row 3): H = s A^-1 - t A^-1.
                double h[4][3] = {};
                for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++)
                        h[i][k] = inv[i][k];
                for (int k = 0; k < 3; k++)
                    h[3][k] = -(m[3][0] * inv[0][k] + m[3][1] * inv[1][k] + m[3][2] * inv[2][k]);
                // The previous view (H + c - c') R', then its projection and jitter.
                double c[3], cp[3];
                CameraPlace(info->view, info->camPos, c);
                CameraPlace(m_ssrPrevView, m_ssrPrevCamPos, cp);
                double q[4][4] = {}; // (s, 1) -> the previous view (x, y, z, 1)
                for (int i = 0; i < 4; i++)
                {
                    for (int j = 0; j < 3; j++)
                    {
                        double v = 0;
                        for (int k = 0; k < 3; k++)
                            v += (h[i][k] + (i == 3 ? c[k] - cp[k] : 0.0)) * m_ssrPrevView[k * 4 + j];
                        q[i][j] = v;
                    }
                    q[i][3] = i == 3 ? 1.0 : 0.0;
                }
                const double pu = m_ssrPrevShift[0], pv = m_ssrPrevShift[1];
                for (int i = 0; i < 4; i++)
                {
                    double x = 0, y = 0, w = 0;
                    for (int j = 0; j < 4; j++)
                    {
                        x += q[i][j] * m_ssrPrevProj[j * 4 + 0];
                        y += q[i][j] * m_ssrPrevProj[j * 4 + 1];
                        w += q[i][j] * m_ssrPrevProj[j * 4 + 3];
                    }
                    out[i] = static_cast<float>(0.5 * x + (0.5 + pu) * w);
                    out[4 + i] = static_cast<float>(-0.5 * y + (0.5 + pv) * w);
                    out[8 + i] = static_cast<float>(w);
                }
                out[12] = 1.0f;
                static const bool partDebug = [] {
                    wchar_t v[8] = {};
                    return GetEnvironmentVariableW(L"CRYMT_SSR_PARTICLES", v, 8) && wcscmp(v, L"2") == 0;
                }();
                out[13] = LastParticleLayer() ? (partDebug ? 2.0f : 1.0f) : 0.0f;
                out[14] = static_cast<float>(m_giFullW);
                out[15] = static_cast<float>(m_giFullH);
            }
        }
    }

    void UpdateFrameTexture(const DlssSceneInfo* info)
    {
        if (!m_jitterTex)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = 16;
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&m_jitterTex))))
            {
                m_jitterTex = nullptr;
                return;
            }
            m_jitterCpu = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_jitterTex, nullptr, m_jitterCpu);
        }
        const GiSettings& s = Gi();
        float data[64] = {};
        data[0] = info->shiftU;
        data[1] = info->shiftV;
        data[2] = 0.0f; // the contact shadows' noise frame: fixed (a noise that moved per frame shimmered on a still view)
        memcpy(&data[4], info->sun, 3 * sizeof(float));
        // 2: the old contact shadows; 3: Bend's screen-space shadows (RunSss; the shader reads them in place of the sss
        // placeholder), unless their pipeline failed.
        data[7] = s.contactShadows && info->sun[3] > 0 ? (s.sss && !m_sssFailed ? 3.0f : 2.0f) : 0.0f;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
            {
                float v = 0;
                for (int k = 0; k < 4; k++)
                    v += info->view[r * 4 + k] * info->proj[k * 4 + c];
                data[8 + c * 4 + r] = v;
            }
        data[24] = s.contactLength;
        data[25] = 0.5f;
        data[26] = 50.0f;
        data[27] = s.contactFoliage ? 1.0f : 0.0f;
        // Texel 7: the cube's brightness on the water; which cube (0 the voxel grid's, 1 the engine's RGBK one, 2 its HDR copy,
        // 3 + L that copy read at mip level L: the debug); the near plane; 2 when the water's SSR is on.
        const EnvView waterCube = WaterEnvCube();
        const bool engineCube = waterCube.kind != 0;
        if (m_engineCubeDumpWanted) // the capture key: the world direction to the sun, to place it in the cube's dump
            Log("capture: sun direction %.3f %.3f %.3f (found %.0f)", info->sun[0], info->sun[1], info->sun[2], info->sun[3]);
        data[28] = engineCube ? EngineCubeScale() : WaterSky();
        data[29] = waterCube.kind == 2 ? (g_giControl.envCubeMip >= 0 ? 3.0f + static_cast<float>(g_giControl.envCubeMip) : 2.0f)
                                       : static_cast<float>(waterCube.kind);
        data[30] = info->nearZ;
        data[31] = WaterSsrOn() && !m_hizFailed ? 2.0f : 0.0f;
        WaterPrevFrame(info, data + 32);
        m_hizNearOverFar = info->farZ > 0 ? info->nearZ / info->farZ : 0.0f;
        // The sun for RunSss: its clip position through the unjittered view-projection (a direction: w 0), moved by the
        // frame's jitter as the depth was drawn (the image shifted +x right, +y down: x + 2 shiftU w, y - 2 shiftV w).
        m_sssSun = info->sun[3] > 0;
        m_sssFar = info->farZ;
        for (int c = 0; c < 4; c++)
            m_sssClip[c] = info->sun[0] * data[8 + c * 4] + info->sun[1] * data[9 + c * 4] + info->sun[2] * data[10 + c * 4];
        m_sssClip[0] += 2 * info->shiftU * m_sssClip[3];
        m_sssClip[1] -= 2 * info->shiftV * m_sssClip[3];
        if (memcmp(data, m_frameLast, sizeof(data)) == 0)
            return;
        const UploadAlloc up = FrameUpload(256, 512);
        if (!up.cpu)
            return;
        memcpy(up.cpu, data, sizeof(data));
        m_barriers.push_back(Transition(m_jitterTex, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
        FlushBarriers();
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m_jitterTex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = up.offset;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 16, 1, 1, 256};
        NoteCopy("jitter", dst, 0, 0, 0, src, nullptr);
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m_barriers.push_back(Transition(m_jitterTex, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
        memcpy(m_frameLast, data, sizeof(data));
    }

    // The Forward+ test (CRYMT_FPLUS=1 or the engine page's Num1): -1 until the panel sets it.
    static bool FPlusOn()
    {
        static const bool env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_FPLUS", v, 8) && v[0] == L'1';
        }();
        const int panel = g_giControl.fplus;
        return panel < 0 ? env : panel > 0;
    }

    // The Forward+ test's texture (the patched fragLib's single-pass light loop reads it, Settings.cpp kFPlusDecl): the
    // frame's point lamps without shadow maps (DlssSceneInfo::fplusLamps) binned into 32-pixel screen tiles by the
    // screen rectangle of their sphere's box (the whole screen when the camera is near it or a corner is behind the
    // camera). Texel 0 (on, tiles across, down, lamps); 1 + 2k, 2 + 2k lamp k's position from the camera and 1 / radius,
    // its colour; 65 the camera; from 128 one per tile (its first index texel, its lamp count); then the indices, 4 a
    // texel. Uploaded when it changed (the rows in use).
    void UpdateFPlus(const DlssSceneInfo* info)
    {
        constexpr UINT kTexW = 1024, kTexH = 128, kTile = 32, kGrid = 128;
        if (!m_fplusTex)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = kTexW;
            d.Height = kTexH;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&m_fplusTex))))
            {
                m_fplusTex = nullptr;
                return;
            }
            m_fplusCpu = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_fplusTex, nullptr, m_fplusCpu);
            Log("Forward+ test texture created (%s)", FPlusOn() ? "on" : "off until Num1 on the engine page");
        }
        const UINT sw = info->fplusWidth, sh = info->fplusHeight;
        const UINT tx = (sw + kTile - 1) / kTile, ty = (sh + kTile - 1) / kTile, tiles = tx * ty;
        const UINT count = std::min<UINT>(info->fplusCount, 32);
        const bool on = FPlusOn() && tiles > 0 && tiles <= 16384;
        m_fplusStats.lamps = count;
        m_fplusStats.onScreen = m_fplusStats.litTiles = m_fplusStats.maxPerTile = m_fplusStats.indices = 0;
        if (!on)
        {
            m_fplusData.assign(kTexW * 4, 0.0f);
        }
        else
        {
            float vp[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    float v = 0;
                    for (int k = 0; k < 4; k++)
                        v += info->view[r * 4 + k] * info->proj[k * 4 + c];
                    vp[r * 4 + c] = v;
                }
            const float* cam = info->fplusCam;
            const float nearZ = std::max(info->nearZ, 0.05f);
            struct Rect
            {
                int x0, y0, x1, y1;
            } rects[32];
            m_fplusCount.assign(tiles, 0);
            for (UINT i = 0; i < count; i++)
            {
                const float* l = info->fplusLamps[i];
                Rect& q = rects[i];
                q = {0, 0, -1, -1};
                const float r = l[3] > 0 ? 1.0f / l[3] : 0.0f;
                if (!(r > 0))
                    continue;
                const float d[3] = {l[0] - cam[0], l[1] - cam[1], l[2] - cam[2]};
                const float reach = r + 2 * nearZ + 0.5f;
                bool full = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < reach * reach;
                float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
                int behind = 0;
                for (int k = 0; k < 8 && !full; k++)
                {
                    const float p[3] = {l[0] + (k & 1 ? r : -r), l[1] + (k & 2 ? r : -r), l[2] + (k & 4 ? r : -r)};
                    float clip[4];
                    for (int c = 0; c < 4; c++)
                        clip[c] = p[0] * vp[c] + p[1] * vp[4 + c] + p[2] * vp[8 + c] + vp[12 + c];
                    if (clip[3] < nearZ * 0.5f)
                    {
                        behind++;
                        continue;
                    }
                    const float u = clip[0] / clip[3] * 0.5f + 0.5f, v = -clip[1] / clip[3] * 0.5f + 0.5f;
                    u0 = std::min(u0, u);
                    u1 = std::max(u1, u);
                    v0 = std::min(v0, v);
                    v1 = std::max(v1, v);
                }
                if (behind == 8)
                    continue; // the whole box behind the camera
                if (full || behind)
                {
                    u0 = v0 = 0.0f;
                    u1 = v1 = 1.0f;
                }
                // A pixel's margin (the jitter, the screen coordinate's rounding).
                u0 -= 1.0f / sw;
                v0 -= 1.0f / sh;
                u1 += 1.0f / sw;
                v1 += 1.0f / sh;
                if (u1 < 0 || v1 < 0 || u0 > 1 || v0 > 1)
                    continue;
                const auto cell = [](float f, UINT n) { return std::min(std::max(static_cast<int>(floorf(f * n)), 0), static_cast<int>(n) - 1); };
                q = {cell(u0, tx), cell(v0, ty), cell(u1, tx), cell(v1, ty)};
                m_fplusStats.onScreen++;
            }
            // At most 4 lamps a tile (the shader's slots: its loop count must not vary per pixel), the strongest at the
            // camera first; a lamp left out of a tile keeps the engine's own copy there.
            UINT order[32];
            float weight[32];
            for (UINT i = 0; i < count; i++)
            {
                const float* l = info->fplusLamps[i];
                const float d[3] = {l[0] - cam[0], l[1] - cam[1], l[2] - cam[2]};
                const float r = l[3] > 0 ? 1.0f / l[3] : 0.0f;
                weight[i] = (l[4] + l[5] + l[6]) * r * r / (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + 1.0f);
                order[i] = i;
            }
            std::sort(order, order + count, [&](UINT a, UINT b) { return weight[a] > weight[b]; });
            for (UINT k = 0; k < count; k++)
            {
                const Rect& q = rects[order[k]];
                for (int y = q.y0; y <= q.y1; y++)
                    for (int x = q.x0; x <= q.x1; x++)
                        if (m_fplusCount[y * tx + x] < 4)
                            m_fplusCount[y * tx + x]++;
            }
            m_fplusStart.resize(tiles);
            UINT next = kGrid + tiles;
            for (UINT t = 0; t < tiles; t++)
            {
                m_fplusStart[t] = next;
                next += (m_fplusCount[t] + 3) / 4;
                if (m_fplusCount[t])
                {
                    m_fplusStats.litTiles++;
                    m_fplusStats.maxPerTile = std::max(m_fplusStats.maxPerTile, m_fplusCount[t]);
                    m_fplusStats.indices += m_fplusCount[t];
                }
            }
            if (next > kTexW * kTexH)
                return; // cannot happen: 16384 tiles + 16384 * 8 index texels fit
            const UINT rows = (next + kTexW - 1) / kTexW;
            m_fplusData.assign(static_cast<size_t>(rows) * kTexW * 4, 0.0f);
            float* o = m_fplusData.data();
            o[0] = 1.0f;
            o[1] = static_cast<float>(tx);
            o[2] = static_cast<float>(ty);
            o[3] = static_cast<float>(count);
            for (UINT i = 0; i < count; i++)
            {
                const float* l = info->fplusLamps[i];
                float* p = o + (1 + 2 * i) * 4;
                p[0] = l[0] - cam[0];
                p[1] = l[1] - cam[1];
                p[2] = l[2] - cam[2];
                p[3] = l[3];
                memcpy(p + 4, l + 4, 4 * sizeof(float));
            }
            memcpy(o + 65 * 4, cam, 4 * sizeof(float));
            for (UINT t = 0; t < tiles; t++)
            {
                o[(kGrid + t) * 4 + 0] = static_cast<float>(m_fplusStart[t]);
                o[(kGrid + t) * 4 + 1] = static_cast<float>(m_fplusCount[t]);
                m_fplusCount[t] = 0; // now the fill position
            }
            for (UINT k = 0; k < count; k++)
            {
                const Rect& q = rects[order[k]];
                for (int y = q.y0; y <= q.y1; y++)
                    for (int x = q.x0; x <= q.x1; x++)
                    {
                        const UINT t = y * tx + x;
                        if (m_fplusCount[t] < static_cast<UINT>(o[(kGrid + t) * 4 + 1]))
                            o[m_fplusStart[t] * 4 + m_fplusCount[t]++] = static_cast<float>(order[k]);
                    }
            }
        }
        if (m_fplusData == m_fplusLast)
            return;
        const UINT rows = static_cast<UINT>(m_fplusData.size() / (kTexW * 4));
        const uint64_t bytes = static_cast<uint64_t>(rows) * kTexW * 16;
        const UploadAlloc up = FrameUpload(bytes, 512);
        if (!up.cpu)
            return;
        memcpy(up.cpu, m_fplusData.data(), bytes);
        m_barriers.push_back(Transition(m_fplusTex, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
        FlushBarriers();
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m_fplusTex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = up.offset;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kTexW, rows, 1, kTexW * 16};
        NoteCopy("fplus", dst, 0, 0, 0, src, nullptr);
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m_barriers.push_back(Transition(m_fplusTex, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
        m_fplusLast = m_fplusData;
        if (m_fplusStats.uploads++ % 1000 == 0)
            Log("Forward+ test: %s, %u lamps, %u on screen, %u of %u tiles lit, max %u a tile, %u indices, %u rows", on ? "on" : "off",
                m_fplusStats.lamps, m_fplusStats.onScreen, m_fplusStats.litTiles, tiles, m_fplusStats.maxPerTile, m_fplusStats.indices, rows);
    }

    void GiFrameCamera(const DlssSceneInfo* info)
    {
        m_pastScene = false;
        if (info->valid && g_jitterPlaceholder)
            UpdateFrameTexture(info);
        if (info->valid && g_fplusPlaceholder)
            UpdateFPlus(info);
        g_wantPassMatrices.store(Gi().on && ((Gi().geometry && (Gi().shadowGeometry || Gi().cubeGeometry || Gi().cubeInject)) ||
                                             (Gi().vfog && Gi().vfogSun > 0)),
                                 std::memory_order_relaxed);
        if (info->reset) // the sun's maps of another place or level (the fog's shadows): their textures may be gone
            for (SunMap& s : m_sunMaps)
                s = SunMap{};
        GiCamera(info);
        m_giInScene = info->valid != 0;
        // A frame whose GI pass did not run leaves its draws: dropped with the first camera of the next frame. Not at
        // every camera: the engine sets the main camera again within a frame (before the z-prepass, the general pass,
        // after the water), and that dropped the z-prepass draws recorded before it - CryMT's own z-prepass draws the
        // solid geometry first - so the GI's voxels got the alpha-tested ones (foliage) and almost no ground, walls or
        // rocks (the user: holes across roads, steps; a capture: 1003 solid draws recorded a frame, 17 voxelized).
        const uint64_t frame = g_presented.load(std::memory_order_relaxed);
        if (frame != m_voxDrawsFrame)
        {
            m_voxDraws.clear();
            m_voxCameraCopies.clear();
            m_voxDrawsFrame = frame;
        }
    }

    // The main camera of the frame being drawn (stream order: before its draws), for the GI pass.
    void GiCamera(const DlssSceneInfo* info)
    {
        if (info->valid)
        {
            memcpy(m_giView, info->view, sizeof(m_giView));
            memcpy(m_giProj, info->proj, sizeof(m_giProj));
            memcpy(m_giCamPos, info->camPos, sizeof(m_giCamPos));
            m_giHaveCamera = true;
        }
    }

    // Full-size pixels per GI texel each way: 2 up to about 0.65 million GI texels (1920x1080 gives 0.52), else 4
    // (5120x1440: 0.46 instead of 1.84; the trace cost 3 ms of GPU there at half size). CRYMT_GI_SCALE=2 or 4 forces it.
    static UINT GiScale(UINT width, UINT height)
    {
        static const int forced = [] {
            wchar_t text[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_GI_SCALE", text, 8) ? _wtoi(text) : 0;
        }();
        if (forced == 2 || forced == 4)
            return static_cast<UINT>(forced);
        return (width / 2) * (height / 2) <= 650000 ? 2 : 4;
    }

    bool EnsureGi(UINT width, UINT height, DXGI_FORMAT format)
    {
        if (m_giColor && width == m_giFullW && height == m_giFullH && format == m_giColorFormat)
            return true;
        DeferRelease(m_giColor);
        DeferRelease(m_giHist[0]);
        DeferRelease(m_giHist[1]);
        DeferRelease(m_giRaw);
        DeferRelease(m_giDist[0]);
        DeferRelease(m_giDist[1]);
        DeferRelease(m_giFull);
        DeferRelease(m_aoRaw);
        DeferRelease(m_aoBlur);
        DeferRelease(m_aoHist[0]);
        DeferRelease(m_aoHist[1]);
        m_giColor = m_giHist[0] = m_giHist[1] = m_giRaw = m_giDist[0] = m_giDist[1] = m_giFull = m_aoRaw = m_aoBlur = nullptr;
        m_aoHist[0] = m_aoHist[1] = nullptr;
        m_aoHistState[0] = m_aoHistState[1] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_giCpu = {};
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                    reinterpret_cast<void**>(&m_giColor));
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // full-size GI (upsample)
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (SUCCEEDED(hr))
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_giFull));
        {
            // The GTAO passes' occlusion, raw and blurred (full size; the half-size mode uses their top-left part).
            D3D12_RESOURCE_DESC ad = d;
            ad.Format = DXGI_FORMAT_R16_FLOAT;
            for (ID3D12Resource** r : {&m_aoRaw})
                if (SUCCEEDED(hr))
                    hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                        reinterpret_cast<void**>(r));
            if (!m_pvol && SUCCEEDED(hr))
            {
                // The particles' occlusion volume: kPvolX x kPvolY x kPvolZ cells, z-slices kPvolTiles across, a row of its place.
                D3D12_RESOURCE_DESC pd = ad;
                pd.Width = kPvolX * kPvolTiles * 2; // fine and coarse side by side
                pd.Height = kPvolY * ((kPvolZ + kPvolTiles - 1) / kPvolTiles) * 2 + 1; // shares, the place row, tints
                pd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; // x the ambient's share, y the sun, z indoors; the last row x the place
                if (SUCCEEDED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &pd, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                             reinterpret_cast<void**>(&m_pvol))))
                {
                    m_pvolCpu = g_srvCpu.Alloc();
                    g.dev->CreateShaderResourceView(m_pvol, nullptr, m_pvolCpu);
                }
                else
                    m_pvol = nullptr;
            }
            ad.Format = DXGI_FORMAT_R16G16_FLOAT;
            for (ID3D12Resource** r : {&m_aoHist[0], &m_aoHist[1]})
                if (SUCCEEDED(hr))
                    hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                        reinterpret_cast<void**>(r));
        }
        const UINT scale = GiScale(width, height);
        d.Width = (width + scale - 1) / scale;
        d.Height = (height + scale - 1) / scale;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (int i = 0; i < 2 && SUCCEEDED(hr); i++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_giHist[i]));
        if (SUCCEEDED(hr))
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_giRaw));
        d.Format = DXGI_FORMAT_R32_FLOAT; // distances, always unordered access (read back as u2 the next frame)
        for (int i = 0; i < 2 && SUCCEEDED(hr); i++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_giDist[i]));
        if (FAILED(hr))
        {
            Log("GI targets %ux%u failed %08lx", width, height, static_cast<unsigned long>(hr));
            return false;
        }
        for (int i = 0; i < 2; i++)
        {
            if (!m_giHistSrv[i].ptr)
                m_giHistSrv[i] = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_giHist[i], nullptr, m_giHistSrv[i]);
            m_giHistState[i] = kSrvState;
        }
        if (!m_giFullSrv.ptr)
            m_giFullSrv = g_srvCpu.Alloc();
        g.dev->CreateShaderResourceView(m_giFull, nullptr, m_giFullSrv);
        m_giFullW = width;
        m_giFullH = height;
        m_giW = static_cast<UINT>(d.Width);
        m_giH = d.Height;
        m_giColorFormat = format;
        m_giReset = true;
        Log("GI targets %ux%u (GI %ux%u)", width, height, m_giW, m_giH);
        return true;
    }

    bool EnsureDepthAlignPipeline()
    {
        if (m_depthAlignPso || m_depthAlignFailed)
            return m_depthAlignPso != nullptr;
        m_depthAlignFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
            return false;
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = compile(kDepthAlignShader, strlen(kDepthAlignShader), "CryMT_DepthAlign", nullptr, nullptr, "AlignDepth", "cs_5_0",
                             1 << 15 /* OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
        if (FAILED(hr))
        {
            Log("depth align shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            if (code)
                code->Release();
            if (errors)
                errors->Release();
            return false;
        }
        if (errors)
            errors->Release();
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 6;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 2;
        rd.pParameters = params;
        ID3DBlob* blob = nullptr;
        ID3DBlob* rootErrors = nullptr;
        hr = g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rootErrors);
        if (SUCCEEDED(hr))
            hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                            reinterpret_cast<void**>(&m_depthAlignRoot));
        if (blob)
            blob->Release();
        if (rootErrors)
            rootErrors->Release();
        if (SUCCEEDED(hr))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = m_depthAlignRoot;
            pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&m_depthAlignPso));
        }
        code->Release();
        m_depthAlignFailed = FAILED(hr);
        Log("depth align pipeline: %s", SUCCEEDED(hr) ? "ready" : "failed");
        return SUCCEEDED(hr);
    }

    // The scene's depth at width x height for the GI: the depth target itself when it has that size (the game); the
    // editor's viewport (viewport 0 when it lies in the target, else the top-left corner) copied into an R32F texture of
    // that size when the depth target is the window's.
    TextureCore* SceneDepth(TextureCore* depth, UINT width, UINT height)
    {
        if (!depth || depth->samples != 1 || !width || !height || depth->width < width || depth->height < height)
            return nullptr;
        if (depth->width == width && depth->height == height)
            return depth;
        if (!EnsureDepthAlignPipeline())
            return nullptr;
        float rect[4] = {0, 0, static_cast<float>(width), static_cast<float>(height)};
        if (m_numViewports)
        {
            const D3D10_VIEWPORT& vp = m_vp10[0];
            if (vp.Width && vp.Height && vp.TopLeftX + vp.Width <= depth->width && vp.TopLeftY + vp.Height <= depth->height)
            {
                rect[0] = static_cast<float>(vp.TopLeftX);
                rect[1] = static_cast<float>(vp.TopLeftY);
                rect[2] = static_cast<float>(vp.Width);
                rect[3] = static_cast<float>(vp.Height);
            }
        }
        if (!m_depthAligned.res || m_depthAligned.width != width || m_depthAligned.height != height)
        {
            DeferRelease(m_depthAligned.res);
            m_depthAligned.res = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = width;
            d.Height = height;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_depthAligned.res))))
            {
                m_depthAligned.res = nullptr;
                return nullptr;
            }
            m_depthAligned.width = width;
            m_depthAligned.height = height;
            m_depthAligned.format = DXGI_FORMAT_R32_FLOAT;
            m_depthAligned.samples = 1;
            m_depthAligned.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (memcmp(m_depthAlignRect, rect, sizeof(rect)) != 0)
        {
            memcpy(m_depthAlignRect, rect, sizeof(rect));
            Log("scene depth for the GI: viewport %.0f,%.0f %.0fx%.0f of the %ux%u depth target -> %ux%u", rect[0], rect[1], rect[2], rect[3],
                depth->width, depth->height, width, height);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(2, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        Require(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(&m_depthAligned, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FlushBarriers();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DepthSrvFormat(depth->format);
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(depth->res, &sd, cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.dev->CreateUnorderedAccessView(m_depthAligned.res, nullptr, &ud, {cpu.ptr + g.srvInc});
        const float constants[6] = {static_cast<float>(width), static_cast<float>(height), rect[0], rect[1], rect[2], rect[3]};
        m_list->SetComputeRootSignature(m_depthAlignRoot);
        m_list->SetComputeRoot32BitConstants(0, 6, constants, 0);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->SetPipelineState(m_depthAlignPso);
        m_list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        RestoreListState();
        return &m_depthAligned;
    }

    bool EnsureGiPipeline()
    {
        if (m_giPso || m_giFailed)
            return m_giPso != nullptr;
        m_giFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
        {
            Log("GI: no shader compiler");
            return false;
        }
        constexpr int kEntries = 14;
        ID3DBlob* code[kEntries] = {};
        ID3DBlob* errors = nullptr;
        const char* entries[kEntries] = {"trace", "filter", "motion", "upsample", "camera", "exposure", "sanitize", "sanitizeOut", "gtao", "gtaoBlur", "gtaoApply",
                                         "pvolume", "envmap", "probes"};
        for (int i = 0; i < kEntries; i++)
            if (FAILED(compile(kGiShader, strlen(kGiShader), "CryMT_GI", nullptr, nullptr, entries[i], "cs_5_0", 1 << 15 /* OPTIMIZATION_LEVEL3 */, 0,
                               &code[i], &errors)))
            {
                Log("GI shader %s: %s", entries[i], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
                return false;
            }
        // The second table: the voxel textures and the probes' atlases (t3..), then u3 the probes' light match (probeMatch).
        D3D12_DESCRIPTOR_RANGE ranges[4] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 3},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kGiVoxTable, 3, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 0, kGiVoxTable}};
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = kGiConstants;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // the voxel grid levels (t3..t6), per axis (t7..t10)
        params[2].DescriptorTable = {2, ranges + 2};
        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        for (int i = 0; i < 3; i++)
        {
            samplers[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            const D3D12_TEXTURE_ADDRESS_MODE mode = i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = mode;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
        }
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3;
        rd.pParameters = params;
        rd.NumStaticSamplers = 3;
        rd.pStaticSamplers = samplers;
        ID3DBlob* blob = nullptr;
        if (FAILED(g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&m_giRoot))))
        {
            Log("GI root signature failed");
            return false;
        }
        blob->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_giRoot;
        HRESULT hr = S_OK;
        ID3D12PipelineState** psos[kEntries] = {&m_giPso, &m_giPsoFilter, &m_mvPso, &m_giPsoUpsample, &m_giPsoCamera, &m_expPso, &m_sanPso,
                                                &m_sanOutPso, &m_aoPso, &m_aoBlurPso, &m_aoApplyPso, &m_pvolPso, &m_envPso, &m_giPsoProbes};
        for (int i = 0; i < kEntries; i++)
        {
            pd.CS = {code[i]->GetBufferPointer(), code[i]->GetBufferSize()};
            if (SUCCEEDED(hr))
                hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(psos[i]));
            code[i]->Release();
        }
        if (SUCCEEDED(hr))
        {
            // The first-person view's GI value (pass camera): 1x1, unordered access but for the upsample.
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_giCam))))
                m_giCam = nullptr;
        }
        Log("GI pipeline: %s", SUCCEEDED(hr) ? "ready" : "failed");
        m_giFailed = FAILED(hr);
        return SUCCEEDED(hr);
    }

    // GI v2's two probe atlases (kGiShader "probes"), 1024 columns a cascade: made when first asked for, 2 x (18 MB light and states + 8 MB
    // distances), zeroed (no probe traced yet).
    bool EnsureProbes()
    {
        if (m_probeDepth[1] || m_probeFailed)
            return m_probeDepth[1] != nullptr;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1024 * kProbeCascades;
        d.Height = 1152;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_RESOURCE_DESC dd = d;
        dd.Width = 1024 * kProbeCascades;
        dd.Height = 1024;
        dd.Format = DXGI_FORMAT_R16G16_FLOAT;
        for (int i = 0; i < 2; i++)
        {
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_probeAtlas[i]))))
            {
                m_probeAtlas[i] = nullptr;
                m_probeFailed = true;
            }
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_probeDepth[i]))))
            {
                m_probeDepth[i] = nullptr;
                m_probeFailed = true;
            }
        }
        if (m_probeFailed)
            for (int i = 0; i < 2; i++)
                for (ID3D12Resource** a : {&m_probeAtlas[i], &m_probeDepth[i]})
                    if (*a)
                    {
                        (*a)->Release();
                        *a = nullptr;
                    }
        if (!m_probeFailed)
        {
            // probeMatch: 16 uints, zeroed (no ratio yet), always in unordered access.
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 64;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_probeMatch))))
            {
                m_probeMatch = nullptr;
                m_probeFailed = true;
            }
        }
        m_probeAtlasState[0] = m_probeAtlasState[1] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Log("GI v2 probes: atlases %s", m_probeFailed ? "failed" : "made");
        return !m_probeFailed;
    }

    // Bend's screen-space shadows (BendSss.h): compiled once with the sample count asked for; root: 16 constants (b0) and
    // a table of t0 the depth, t1 the foliage mask, t2 the hardware depth (the first-person view), u0 the result.
    bool EnsureSssPipeline()
    {
        if (m_sssPso || m_sssFailed)
            return m_sssPso != nullptr;
        m_sssFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
            return false;
        char samples[16];
        snprintf(samples, sizeof(samples), "%d", Gi().sssSamples);
        const D3D_SHADER_MACRO macros[] = {{"SAMPLE_COUNT", samples}, {nullptr, nullptr}};
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = compile(kBendSssShader, strlen(kBendSssShader), "CryMT_BendSss", macros, nullptr, "main", "cs_5_0", 1 << 15 /* OPTIMIZATION_LEVEL3 */, 0,
                             &code, &errors);
        if (FAILED(hr))
        {
            Log("screen-space shadows shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            if (code)
                code->Release();
            if (errors)
                errors->Release();
            return false;
        }
        if (errors)
            errors->Release();
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 16;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 2;
        rd.pParameters = params;
        ID3DBlob* blob = nullptr;
        ID3DBlob* rootErrors = nullptr;
        hr = g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rootErrors);
        if (SUCCEEDED(hr))
            hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                            reinterpret_cast<void**>(&m_sssRoot));
        if (blob)
            blob->Release();
        if (rootErrors)
            rootErrors->Release();
        if (SUCCEEDED(hr))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = m_sssRoot;
            pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&m_sssPso));
        }
        code->Release();
        m_sssFailed = FAILED(hr);
        Log("screen-space shadows pipeline: %s (%s samples)", SUCCEEDED(hr) ? "ready" : "failed", samples);
        return SUCCEEDED(hr);
    }

    // Bend's screen-space shadows for the sun from the frame's z-prepass depth (its R32F target: view depth / far), before
    // the deferred shadow pass reads them: 1 to 8 dispatches by the sun's place on screen (Bend::BuildDispatchList), each
    // 64-pixel run of a line towards the sun sharing its depth samples. The depth goes in as 1 / linear depth (BendSss.h).
    void RunSss()
    {
        TextureCore* z = m_zTargetFrame == m_frame ? m_zTarget : nullptr;
        if (!z || !z->res || z->samples != 1 || !z->width || !z->height)
        {
            m_stats.sssNoDepth++;
            return;
        }
        if (!EnsureSssPipeline())
            return;
        const UINT w = z->width, h = z->height;
        if (!m_sss.res || m_sss.width != w || m_sss.height != h)
        {
            DeferRelease(m_sss.res);
            m_sss.res = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = w;
            d.Height = h;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R8_UNORM;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_sss.res))))
            {
                m_sss.res = nullptr;
                m_sss.width = m_sss.height = 0;
                return;
            }
            m_sss.width = w;
            m_sss.height = h;
            m_sss.format = DXGI_FORMAT_R8_UNORM;
            m_sss.samples = 1;
            m_sss.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (!m_sssSrv.ptr)
                m_sssSrv = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_sss.res, nullptr, m_sssSrv);
            Log("screen-space shadows %ux%u", w, h);
        }
        TextureCore* const bendOut = &m_sss;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(4, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        const GiSettings& s = Gi();
        // The scene's depth buffer bound for the deferred shadow pass (stencil): the first-person view lies under 0.1.
        TextureCore* hw = m_dsv ? static_cast<TextureCore*>(m_dsv->info) : nullptr;
        if (hw && (hw->width != w || hw->height != h || hw->samples != 1 || !hw->res))
            hw = nullptr;
        // The foliage mask: whether grass and leaves cast (CRYMT_CONTACTFOLIAGE=0: they do not); skin (HumanSkin's
        // z-prepass marks it 0.3) receives none (the user, 2026-11-15: for now) - so bound whenever it was written.
        Srv12* foliage = m_foliageSrv && m_foliageFrame == m_frame ? m_foliageSrv : nullptr;
        Require(z, kSrvState);
        if (foliage)
            Require(foliage->info, kSrvState);
        if (hw)
            Require(hw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(bendOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FlushBarriers();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(z->res, &sd, cpu);
        if (foliage)
            foliage->Refresh();
        g.dev->CopyDescriptorsSimple(1, {cpu.ptr + g.srvInc}, foliage ? foliage->cpu : g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (hw)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC hd = sd;
            hd.Format = DepthSrvFormat(hw->format);
            g.dev->CreateShaderResourceView(hw->res, &hd, {cpu.ptr + 2 * g.srvInc});
        }
        else
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + 2 * g.srvInc}, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.dev->CreateUnorderedAccessView(bendOut->res, nullptr, &ud, {cpu.ptr + 3 * g.srvInc});

        float clip[4] = {m_sssClip[0], m_sssClip[1], m_sssClip[2], m_sssClip[3]};
        int size[2] = {static_cast<int>(w), static_cast<int>(h)}, lo[2] = {0, 0};
        Bend::DispatchList list = Bend::BuildDispatchList(clip, size, lo, size, false, 64);
        list.LightCoordinate_Shader[2] = 0; // the depth is 1 / linear depth: the sun, at infinity, at 0
        uint32_t k[16] = {};
        const auto put = [&k](int i, float v) { memcpy(&k[i], &v, sizeof(v)); };
        for (int i = 0; i < 4; i++)
            put(i, list.LightCoordinate_Shader[i]);
        k[6] = w;
        k[7] = h;
        put(8, s.sssThickness);
        put(9, s.sssEdge);
        put(10, s.sssContrast);
        put(11, m_sssFar > 0 ? m_sssFar : 1000.0f);
        put(12, s.sssReach);
        put(13, foliage && !s.contactFoliage ? 0.0f : 1.0f);
        put(14, s.sssNear);
        put(15, hw ? s.sssWeaponThickness : s.sssThickness); // the null view reads 0: all "first-person" without the buffer
        m_list->SetComputeRootSignature(m_sssRoot);
        m_list->SetPipelineState(m_sssPso);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        for (int i = 0; i < list.DispatchCount; i++)
        {
            const Bend::DispatchData& d = list.Dispatch[i];
            memcpy(&k[4], d.WaveOffset_Shader, 2 * sizeof(int));
            m_list->SetComputeRoot32BitConstants(0, 16, k, 0);
            m_list->Dispatch(d.WaveCount[0], d.WaveCount[1], d.WaveCount[2]);
        }
        if (m_giDumpWanted) // the capture key: the result as logs\play\<tag>_sss.pgm, the foliage mask as <tag>_foliage.pgm
        {
            Require(&m_sss, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (foliage)
                Require(foliage->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            RecordDump(m_sss.res, "sss", 4);
            if (foliage)
                RecordDump(foliage->info->res, "foliage", 4);
            if (m_engineCube) // the engine's environment cube: <tag>_envcube<face>.ppm (+x -x +y -y +z -z)
            {
                Require(m_engineCube->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                FlushBarriers();
                static const char* const faces[6] = {"envcube0", "envcube1", "envcube2", "envcube3", "envcube4", "envcube5"};
                for (UINT face = 0; face < 6; face++)
                    RecordDump(m_engineCube->info->res, faces[face], 6, face);
            }
        }
        RestoreListState();
        m_sssFrame = m_frame;
        m_stats.sssRuns++;
        m_stats.sssDispatches += list.DispatchCount;
        static int logged;
        if (logged++ < 3)
            Log("screen-space shadows: sun at %.1f, %.1f px (w %+.0f), %d dispatches, far %.1f m, %ux%u", list.LightCoordinate_Shader[0],
                list.LightCoordinate_Shader[1], list.LightCoordinate_Shader[3], list.DispatchCount, m_sssFar, w, h);
    }

    // The depth pyramid (kHizShader): R32F with 7 levels at the z-prepass target's size, all levels as unordered access
    // while it is built, then read by the water's pixel shader (CryMT_HiZSampler).
    bool EnsureHizPipeline()
    {
        if (m_hizInitPso || m_hizFailed)
            return m_hizInitPso != nullptr;
        m_hizFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
            return false;
        ID3DBlob* code[2] = {};
        const char* entries[2] = {"hizInit", "hizDown"};
        for (int i = 0; i < 2; i++)
        {
            ID3DBlob* errors = nullptr;
            if (FAILED(compile(kHizShader, strlen(kHizShader), "CryMT_HiZ", nullptr, nullptr, entries[i], "cs_5_0", 1 << 15, 0, &code[i], &errors)))
            {
                Log("depth pyramid shader %s: %s", entries[i], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
                if (errors)
                    errors->Release();
                return false;
            }
            if (errors)
                errors->Release();
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 6;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 2;
        rd.pParameters = params;
        ID3DBlob* blob = nullptr;
        ID3DBlob* rootErrors = nullptr;
        HRESULT hr = g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rootErrors);
        if (SUCCEEDED(hr))
            hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                            reinterpret_cast<void**>(&m_hizRoot));
        if (blob)
            blob->Release();
        if (rootErrors)
            rootErrors->Release();
        ID3D12PipelineState** psos[2] = {&m_hizInitPso, &m_hizDownPso};
        for (int i = 0; i < 2; i++)
        {
            if (SUCCEEDED(hr))
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = m_hizRoot;
                pd.CS = {code[i]->GetBufferPointer(), code[i]->GetBufferSize()};
                hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(psos[i]));
            }
            code[i]->Release();
        }
        if (FAILED(hr))
        {
            if (m_hizInitPso)
                m_hizInitPso->Release();
            m_hizInitPso = nullptr;
        }
        m_hizFailed = FAILED(hr);
        Log("depth pyramid pipeline: %s", SUCCEEDED(hr) ? "ready" : "failed");
        return SUCCEEDED(hr);
    }

    void RunHiz()
    {
        TextureCore* z = m_zTargetFrame == m_frame ? m_zTarget : nullptr;
        if (!z || !z->res || z->samples != 1 || !z->width || !z->height || !EnsureHizPipeline())
            return;
        const UINT w = z->width, h = z->height;
        if (!m_hiz.res || m_hiz.width != w || m_hiz.height != h)
        {
            DeferRelease(m_hiz.res);
            m_hiz.res = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = w;
            d.Height = h;
            d.DepthOrArraySize = 1;
            d.MipLevels = kHizLevels;
            d.Format = DXGI_FORMAT_R32_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_hiz.res))))
            {
                m_hiz.res = nullptr;
                m_hiz.width = m_hiz.height = 0;
                return;
            }
            m_hiz.width = w;
            m_hiz.height = h;
            m_hiz.format = DXGI_FORMAT_R32_FLOAT;
            m_hiz.samples = 1;
            m_hiz.mips = kHizLevels;
            m_hiz.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (!m_hizSrv.ptr)
                m_hizSrv = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_hiz.res, nullptr, m_hizSrv);
            for (UINT m = 0; m < kHizLevels; m++)
            {
                if (!m_hizUav[m].ptr)
                    m_hizUav[m] = g_srvCpu.Alloc();
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_FLOAT;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                ud.Texture2D.MipSlice = m;
                g.dev->CreateUnorderedAccessView(m_hiz.res, nullptr, &ud, m_hizUav[m]);
            }
            Log("depth pyramid %ux%u, %u levels", w, h, kHizLevels);
        }
        TextureCore* hw = m_dsv ? static_cast<TextureCore*>(m_dsv->info) : nullptr;
        if (hw && (hw->width != w || hw->height != h || hw->samples != 1 || !hw->res))
            hw = nullptr;
        Require(z, kSrvState);
        if (hw)
            Require(hw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu0;
        while (!m_srvRing.TryAlloc(4 * kHizLevels, cpu0, gpu0))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        Require(&m_hiz, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FlushBarriers();
        m_list->SetComputeRootSignature(m_hizRoot);
        UINT sw = w, sh = h;
        for (UINT m = 0; m < kHizLevels; m++)
        {
            const UINT dw = std::max(1u, w >> m), dh = std::max(1u, h >> m);
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu{cpu0.ptr + 4 * m * g.srvInc};
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu{gpu0.ptr + 4 * m * g.srvInc};
            if (m == 0)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R32_FLOAT;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                g.dev->CreateShaderResourceView(z->res, &sd, cpu);
                if (hw)
                {
                    sd.Format = DepthSrvFormat(hw->format);
                    g.dev->CreateShaderResourceView(hw->res, &sd, {cpu.ptr + g.srvInc});
                }
                else
                    g.dev->CopyDescriptorsSimple(1, {cpu.ptr + g.srvInc}, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            else
            {
                g.dev->CopyDescriptorsSimple(1, cpu, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                g.dev->CopyDescriptorsSimple(1, {cpu.ptr + g.srvInc}, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + 2 * g.srvInc}, m_hizUav[m], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + 3 * g.srvInc}, m_hizUav[m ? m - 1 : 0], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            uint32_t k[6] = {dw, dh, sw, sh, 0, hw ? 1u : 0u};
            memcpy(&k[4], &m_hizNearOverFar, sizeof(float));
            const float one = 1.0f, zero = 0.0f;
            memcpy(&k[5], hw ? &one : &zero, sizeof(float));
            if (m > 0)
            {
                D3D12_RESOURCE_BARRIER uav{};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.UAV.pResource = m_hiz.res;
                m_list->ResourceBarrier(1, &uav);
            }
            m_list->SetPipelineState(m ? m_hizDownPso : m_hizInitPso);
            m_list->SetComputeRoot32BitConstants(0, 6, k, 0);
            m_list->SetComputeRootDescriptorTable(1, gpu);
            m_list->Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
            sw = dw;
            sh = dh;
        }
        RestoreListState();
        m_hizFrame = m_frame;
        m_stats.hizRuns++;
    }

    bool EnsureVfogPipeline()
    {
        if (m_vfogPso || m_vfogFailed)
            return m_vfogPso != nullptr;
        m_vfogFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
            return false;
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(compile(kVfogShader, strlen(kVfogShader), "CryMT_VFog", nullptr, nullptr, "vfog", "cs_5_0", 1 << 15, 0, &code, &errors)))
        {
            Log("volumetric fog shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            if (errors)
                errors->Release();
            return false;
        }
        if (errors)
            errors->Release();
        // t0 the particles' volumes, t1..t4 the sun's cascades; u0 the volume, u1 the fog pass's parameters; b1 the
        // cascades' matrices.
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1 + kVfogCascades, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 1 + kVfogCascades}};
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 32;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor.ShaderRegister = 1;
        D3D12_STATIC_SAMPLER_DESC ss[2]{};
        ss[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[0].MaxLOD = D3D12_FLOAT32_MAX;
        ss[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ss[1] = ss[0];
        ss[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ss[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // lit: the point no deeper than the map's nearest caster
        ss[1].ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3;
        rd.pParameters = params;
        rd.NumStaticSamplers = 2;
        rd.pStaticSamplers = ss;
        ID3DBlob* blob = nullptr;
        ID3DBlob* rootErrors = nullptr;
        HRESULT hr = g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rootErrors);
        if (SUCCEEDED(hr))
            hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                            reinterpret_cast<void**>(&m_vfogRoot));
        if (blob)
            blob->Release();
        if (rootErrors)
            rootErrors->Release();
        if (SUCCEEDED(hr))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = m_vfogRoot;
            pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&m_vfogPso));
        }
        code->Release();
        if (FAILED(hr))
        {
            Log("volumetric fog pipeline: 0x%08x", static_cast<unsigned>(hr));
            m_vfogPso = nullptr;
            return false;
        }
        m_vfogFailed = false;
        return true;
    }

    // A null 3D view (the fog volume's slot without a volume: reads zero), made on first use.
    D3D12_CPU_DESCRIPTOR_HANDLE VfogNull3d()
    {
        if (!m_vfogNull3d.ptr)
        {
            m_vfogNull3d = g_srvCpu.Alloc();
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture3D.MipLevels = 1;
            g.dev->CreateShaderResourceView(nullptr, &d, m_vfogNull3d);
        }
        return m_vfogNull3d;
    }

    // An engine constant the bound pixel shader declares (Shader12::fogSlot), from its dynamic constant buffer; null when
    // not there.
    const float* PsFogConstant(int i, UINT bytes) const
    {
        const int slot = m_ps ? m_ps->fogSlot[i] : -1;
        if (slot < 0 || slot >= static_cast<int>(kRootCbSlots[kPS]))
            return nullptr;
        const Buffer12* b = m_stage[kPS].cb[slot];
        if (!b || !b->Dynamic() || !b->slice.cpu || m_ps->fogOffset[i] + bytes > b->desc.ByteWidth)
            return nullptr;
        return reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(b->slice.cpu) + m_ps->fogOffset[i]);
    }

    // Volumetric fog, stage 1 (kVfogShader): before the first draw of the scene's fog pass (the patched FogPass reads the
    // vfog placeholder), with the engine's fog constants from that pass's buffers and this frame's GI volumes (pvolume).
    // Other targets' fog passes (the water's reflection, the engine cube's faces) keep the engine's fog.
    void RunVfog()
    {
        const GiSettings& s = Gi();
        const auto* rt = m_rtv[0] ? static_cast<const TextureCore*>(m_rtv[0]->info) : nullptr;
        if (!rt || rt->width != m_giFullW || rt->height != m_giFullH || !SceneColourLike(m_rtv[0]->info))
        {
            m_stats.vfogOtherTarget++;
            return;
        }
        m_vfogTriedFrame = m_frame;
        if (!s.on || !s.vfog)
            return;
        if (m_pvolFrame != m_frame || !m_pvolCpu.ptr)
        {
            m_stats.vfogNoVolume++;
            return;
        }
        const float* params = PsFogConstant(0, 16);
        if (!params)
        {
            m_stats.vfogNoConstants++;
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log("volumetric fog: the fog pass's vfParams not found (slot %d, offset %u)", m_ps ? m_ps->fogSlot[0] : -1, m_ps ? m_ps->fogOffset[0] : 0);
            }
            return;
        }
        if (!EnsureVfogPipeline())
            return;
        const UINT gx = (rt->width + 15) / 16, gy = (rt->height + 15) / 16;
        if (!m_vfog.res || m_vfog.width != gx || m_vfog.height != gy)
        {
            DeferRelease(m_vfog.res);
            m_vfog.res = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            d.Width = gx;
            d.Height = gy;
            d.DepthOrArraySize = static_cast<UINT16>(kVfogSlices);
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_vfog.res))))
            {
                m_vfog.res = nullptr;
                m_vfog.width = m_vfog.height = 0;
                return;
            }
            m_vfog.dim = D3D10_RESOURCE_DIMENSION_TEXTURE3D;
            m_vfog.width = gx;
            m_vfog.height = gy;
            m_vfog.depthOrArray = kVfogSlices;
            m_vfog.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_vfog.samples = 1;
            m_vfog.mips = 1;
            m_vfog.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (!m_vfogSrv.ptr)
                m_vfogSrv = g_srvCpu.Alloc();
            if (!m_vfogUav.ptr)
                m_vfogUav = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_vfog.res, nullptr, m_vfogSrv);
            g.dev->CreateUnorderedAccessView(m_vfog.res, nullptr, nullptr, m_vfogUav);
        }
        if (!m_vfogParam.res)
        {
            // The fog pass's parameters (the pass writes them): 1x1 RGBA32F.
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = 1;
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_vfogParam.res))))
            {
                m_vfogParam.res = nullptr;
                return;
            }
            m_vfogParam.dim = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            m_vfogParam.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            m_vfogParam.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            m_vfogParamSrv = g_srvCpu.Alloc();
            m_vfogParamUav = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_vfogParam.res, nullptr, m_vfogParamSrv);
            g.dev->CreateUnorderedAccessView(m_vfogParam.res, nullptr, nullptr, m_vfogParamUav);
            Log("volumetric fog volume %ux%ux%u and its parameters", gx, gy, kVfogSlices);
        }
        float toWorld[16];
        RigidInverse(m_giView, toWorld);
        // Stage 2: the sun's cascades known (drawn within the last 600 frames: the cached ones are redrawn as the camera
        // moves), finest first (the width they cover at the eye), up to kVfogCascades; their matrices for positions from
        // the camera (in double: the light's w is ~1e6), the depth column over the light's far distance, 0.25 m bias.
        struct Pick
        {
            int index;
            double width;
        };
        Pick picks[kSunMaps];
        int picked = 0;
        const double eye[3] = {toWorld[12], toWorld[13], toWorld[14]};
        for (int i = 0; i < kSunMaps && s.vfogSun > 0; i++)
        {
            const SunMap& sm = m_sunMaps[i];
            if (!sm.map || !sm.map->res || sm.frame + 600 < m_frame)
                continue;
            double w = sm.mv[15], sx = 0;
            for (int r = 0; r < 3; r++)
            {
                w += eye[r] * sm.mv[r * 4 + 3];
                sx += static_cast<double>(sm.mv[r * 4]) * sm.mv[r * 4];
            }
            if (!(w > 0) || !(sx > 0))
                continue;
            picks[picked++] = {i, w / std::sqrt(sx)};
        }
        std::sort(picks, picks + picked, [](const Pick& a, const Pick& b) { return a.width < b.width; });
        picked = std::min(picked, kVfogCascades);
        D3D12_GPU_VIRTUAL_ADDRESS cascadeCb = 0;
        {
            const UploadAlloc up = FrameUpload(512, 256);
            if (up.cpu)
            {
                auto* f = reinterpret_cast<float*>(up.cpu);
                memset(f, 0, 512);
                for (int c = 0; c < picked; c++)
                {
                    const SunMap& sm = m_sunMaps[picks[c].index];
                    for (int j = 0; j < 4; j++)
                    {
                        double t = sm.mv[12 + j];
                        for (int r = 0; r < 3; r++)
                            t += eye[r] * sm.mv[r * 4 + j];
                        if (j == 2)
                            t -= 0.25; // bias (m)
                        const double scale = j == 2 ? 1.0 / sm.lightFar : 1.0;
                        for (int r = 0; r < 3; r++)
                            f[c * 16 + r * 4 + j] = static_cast<float>(sm.mv[r * 4 + j] * scale);
                        f[c * 16 + 12 + j] = static_cast<float>(t * scale);
                    }
                }
                f[64] = static_cast<float>(picked);
                cascadeCb = up.res->GetGPUVirtualAddress() + up.offset;
            }
            else
                picked = 0;
        }
        m_vfogCascadesUsed = static_cast<uint32_t>(picked);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(3 + kVfogCascades, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        g.dev->CopyDescriptorsSimple(1, cpu, m_pvolCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int c = 0; c < kVfogCascades; c++)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE at{cpu.ptr + (1 + c) * g.srvInc};
            if (c < picked)
            {
                ResourceInfo* map = m_sunMaps[picks[c].index].map;
                const auto* t = static_cast<const TextureCore*>(map);
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DepthSrvFormat(t->format);
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                g.dev->CreateShaderResourceView(map->res, &sd, at);
                Require(map, kSrvState);
            }
            else
                g.dev->CopyDescriptorsSimple(1, at, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        g.dev->CopyDescriptorsSimple(1, {cpu.ptr + (1 + kVfogCascades) * g.srvInc}, m_vfogUav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g.dev->CopyDescriptorsSimple(1, {cpu.ptr + (2 + kVfogCascades) * g.srvInc}, m_vfogParamUav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        float k[32] = {};
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 4; i++)
                k[j * 4 + i] = toWorld[i * 4 + j];
        memcpy(k + 12, params, 4 * sizeof(float));
        k[16] = static_cast<float>(gx);
        k[17] = static_cast<float>(gy);
        k[18] = static_cast<float>(kVfogSlices);
        k[19] = kVfogReach;
        k[20] = 1.0f / m_giProj[0];
        k[21] = 1.0f / m_giProj[5];
        k[22] = s.vfogGi;
        k[23] = static_cast<float>(s.vfog);
        k[24] = s.vfogOutdoor; // the fog's own darkening outdoors (not the particles' particleOutdoor)
        k[25] = s.particleMax;
        k[26] = picked > 0 ? s.vfogSun : 0.0f;
        // The near air: s.vfogHaze of the light taken over its reach (its depth there is 0.75 x reach at full density).
        const float haze = s.vfog == 1 ? std::min(std::max(s.vfogHaze, 0.0f), 0.9f) : 0.0f;
        k[28] = haze > 0 ? -std::log2(1.0f - haze) / (0.75f * s.vfogHazeReach) : 0.0f;
        k[29] = s.vfogHazeReach;
        k[30] = static_cast<float>(s.vfogSamples); // the sun's shadow sub-samples per cell
        Require(&m_vfog, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Require(&m_vfogParam, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FlushBarriers();
        m_list->SetComputeRootSignature(m_vfogRoot);
        m_list->SetPipelineState(m_vfogPso);
        m_list->SetComputeRoot32BitConstants(0, 32, k, 0);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        if (cascadeCb)
            m_list->SetComputeRootConstantBufferView(2, cascadeCb);
        m_list->Dispatch((gx + 7) / 8, (gy + 7) / 8, 1);
        Require(&m_vfogParam, kSrvState);
        Require(&m_vfog, kSrvState); // read by the frame's fog pass draws from here on
        FlushBarriers();
        RestoreListState();
        m_vfogFrame = m_frame;
        m_vfogTarget = m_rtv[0]->info;
        m_stats.vfogRuns++;
        if (picked > 0)
        {
            m_stats.vfogCascadeFrames++;
            m_stats.vfogCascadeSum += static_cast<uint64_t>(picked);
        }
        // The fog's constants in the log (the sun's and the fog colour's parts of its light): the first frame, then every
        // 3600th.
        static const UINT kBytes[4] = {16, 12, 12, 12};
        for (int i = 0; i < 4; i++)
            if (const float* f = PsFogConstant(i, kBytes[i]))
                memcpy(m_vfogConstants[i], f, kBytes[i]);
        if (m_stats.vfogRuns % 3600 == 1)
        {
            const float(*c)[4] = m_vfogConstants;
            Log("volumetric fog: vfParams %g %g %g %g, fog colour %g %g %g, Mie %g %g %g, Rayleigh %g %g %g (slots %d %d %d %d)", c[0][0], c[0][1],
                c[0][2], c[0][3], c[1][0], c[1][1], c[1][2], c[2][0], c[2][1], c[2][2], c[3][0], c[3][1], c[3][2], m_ps->fogSlot[0], m_ps->fogSlot[1],
                m_ps->fogSlot[2], m_ps->fogSlot[3]);
        }
    }

    bool EnsureCubePipeline()
    {
        if (m_cubeDecodePso || m_cubeFailed)
            return m_cubeDecodePso != nullptr;
        m_cubeFailed = true;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{DXGI_FORMAT_R16G16B16A16_FLOAT, {}, {}};
        if (FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) || !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
        {
            Log("engine cube in HDR: no typed UAV loads of RGBA16F here, the water reads the engine's RGBK cube as it is");
            return false;
        }
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
            return false;
        ID3DBlob* code[3] = {};
        const char* entries[3] = {"cubeDecode", "cubeDown", "cubePrefilter"};
        for (int i = 0; i < 3; i++)
        {
            ID3DBlob* errors = nullptr;
            if (FAILED(compile(kCubeShader, strlen(kCubeShader), "CryMT_Cube", nullptr, nullptr, entries[i], "cs_5_0", 1 << 15, 0, &code[i], &errors)))
            {
                Log("engine cube shader %s: %s", entries[i], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
                if (errors)
                    errors->Release();
                for (int j = 0; j < i; j++)
                    code[j]->Release();
                return false;
            }
            if (errors)
                errors->Release();
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 8;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        D3D12_STATIC_SAMPLER_DESC linear{};
        linear.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linear.MaxLOD = D3D12_FLOAT32_MAX;
        linear.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 2;
        rd.pParameters = params;
        rd.NumStaticSamplers = 1;
        rd.pStaticSamplers = &linear;
        ID3DBlob* blob = nullptr;
        ID3DBlob* rootErrors = nullptr;
        HRESULT hr = g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rootErrors);
        if (SUCCEEDED(hr))
            hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                            reinterpret_cast<void**>(&m_cubeRoot));
        if (blob)
            blob->Release();
        if (rootErrors)
            rootErrors->Release();
        ID3D12PipelineState** psos[3] = {&m_cubeDecodePso, &m_cubeDownPso, &m_cubeEncodePso};
        for (int i = 0; i < 3; i++)
        {
            if (SUCCEEDED(hr))
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = m_cubeRoot;
                pd.CS = {code[i]->GetBufferPointer(), code[i]->GetBufferSize()};
                hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(psos[i]));
            }
            code[i]->Release();
        }
        if (FAILED(hr) && m_cubeDecodePso)
        {
            m_cubeDecodePso->Release();
            m_cubeDecodePso = nullptr;
        }
        m_cubeFailed = FAILED(hr);
        Log("engine cube in HDR: pipeline %s", SUCCEEDED(hr) ? "ready" : "failed");
        return SUCCEEDED(hr);
    }

    // After the engine drew a face of its cube (the "CryMT engine cube end" mark): that face decoded into the HDR copy
    // (made at the engine cube's size, all mip levels), mixed with the old one unless fresh, and its levels made again.
    void RunCubeUpdate(int face, bool fresh)
    {
        ResourceInfo* srcInfo = m_engineCubeRes ? Info(m_engineCubeRes) : nullptr;
        if (!srcInfo || !srcInfo->res || face < 0 || face > 5 || !EnsureCubePipeline())
            return;
        const D3D12_RESOURCE_DESC sd = srcInfo->res->GetDesc();
        const UINT size = static_cast<UINT>(sd.Width);
        UINT mips = 1;
        while (mips < 12 && (size >> mips) > 0)
            mips++;
        if (!m_hdrCube.res || m_hdrCube.width != size)
        {
            DeferRelease(m_hdrCube.res);
            m_hdrCube.res = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = size;
            d.Height = size;
            d.DepthOrArraySize = 6;
            d.MipLevels = static_cast<UINT16>(mips);
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_hdrCube.res))))
            {
                m_hdrCube.res = nullptr;
                m_hdrCube.width = 0;
                Log("engine cube in HDR: no %ux%u texture", size, size);
                m_cubeFailed = true;
                return;
            }
            m_hdrCube.width = m_hdrCube.height = size;
            m_hdrCube.depthOrArray = 6;
            m_hdrCube.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_hdrCube.samples = 1;
            m_hdrCube.mips = mips;
            m_hdrCube.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (!m_hdrCubeSrv.ptr)
                m_hdrCubeSrv = g_srvCpu.Alloc();
            D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            vd.TextureCube.MipLevels = mips;
            g.dev->CreateShaderResourceView(m_hdrCube.res, &vd, m_hdrCubeSrv);
            for (UINT m = 0; m < mips; m++)
            {
                if (!m_hdrCubeUav[m].ptr)
                    m_hdrCubeUav[m] = g_srvCpu.Alloc();
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                ud.Texture2DArray = {m, 0, 6, 0};
                g.dev->CreateUnorderedAccessView(m_hdrCube.res, nullptr, &ud, m_hdrCubeUav[m]);
            }
            for (bool& f : m_hdrFaceValid)
                f = false;
            Log("engine cube in HDR: %ux%u, %u levels", size, size, mips);
        }
        const float blend = fresh || !m_hdrFaceValid[face] ? 1.0f : EngineCubeBlend();
        Require(srcInfo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(&m_hdrCube, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu0;
        while (!m_srvRing.TryAlloc(4 * mips, cpu0, gpu0)) // per level: t0 the engine's cube (level 0), t1 the level above, u0, u1
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        FlushBarriers();
        m_list->SetComputeRootSignature(m_cubeRoot);
        for (UINT m = 0; m < mips; m++)
        {
            const UINT dsize = std::max(1u, size >> m);
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu{cpu0.ptr + 4 * m * g.srvInc};
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu{gpu0.ptr + 4 * m * g.srvInc};
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + 3 * g.srvInc}, m_hdrCubeUav[m], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); // u1: unused here
            if (m == 0)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                         : sd.Format;
                vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                vd.Texture2DArray = {0, 1, 0, 6, 0, 0.0f};
                g.dev->CreateShaderResourceView(srcInfo->res, &vd, cpu);
            }
            else
                g.dev->CopyDescriptorsSimple(1, cpu, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (m > 0)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC pv{};
                pv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                pv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                pv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                pv.TextureCube = {m - 1, 1, 0.0f};
                g.dev->CreateShaderResourceView(m_hdrCube.res, &pv, {cpu.ptr + g.srvInc});
            }
            else
                g.dev->CopyDescriptorsSimple(1, {cpu.ptr + g.srvInc}, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + 2 * g.srvInc}, m_hdrCubeUav[m], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            uint32_t k[8] = {dsize, static_cast<uint32_t>(face), 0, 0, 0, 0, 0, 0};
            memcpy(&k[2], &blend, sizeof(float));
            const float tanHalf = g_engineCubeTanHalf.load();
            memcpy(&k[3], &tanHalf, sizeof(float));
            // The level above (its six subresources) read through a view: out of unordered access while this level is made.
            D3D12_RESOURCE_BARRIER above[6]{};
            if (m > 0)
                for (UINT f = 0; f < 6; f++)
                {
                    above[f] = Transition(m_hdrCube.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    above[f].Transition.Subresource = (m - 1) + f * mips;
                }
            if (m > 0)
                m_list->ResourceBarrier(6, above);
            m_list->SetPipelineState(m ? m_cubeDownPso : m_cubeDecodePso);
            m_list->SetComputeRoot32BitConstants(0, 8, k, 0);
            m_list->SetComputeRootDescriptorTable(1, gpu);
            m_list->Dispatch((dsize + 7) / 8, (dsize + 7) / 8, 6); // level 0 too: the faces' edges blend from their neighbours
            if (m > 0)
            {
                for (D3D12_RESOURCE_BARRIER& t : above)
                    std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
                m_list->ResourceBarrier(6, above);
            }
        }
        Require(&m_hdrCube, kSrvState); // read by the water from now on (its table may not be rebuilt)
        FlushBarriers();
        if (EnvEngineOn())
            EncodeMaterialsCube(size, mips);
        RestoreListState();
        m_hdrFaceValid[face] = true;
        m_hdrCubeRuns++;
        m_stats.engineCubeHdrRuns++;
    }

    // A materials' copy at the HDR copy's size and levels (RGBA16F, unordered access), with its cube view and level UAVs.
    bool EnsureMaterialsCube(TextureCore& t, D3D12_CPU_DESCRIPTOR_HANDLE& srv, D3D12_CPU_DESCRIPTOR_HANDLE* uav, UINT size, UINT mips)
    {
        if (t.res && t.width == size)
            return true;
        DeferRelease(t.res);
        t.res = nullptr;
        m_ldrCubeReady = false;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = size;
        d.Height = size;
        d.DepthOrArraySize = 6;
        d.MipLevels = static_cast<UINT16>(mips);
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  __uuidof(ID3D12Resource), reinterpret_cast<void**>(&t.res))))
        {
            t.res = nullptr;
            t.width = 0;
            return false;
        }
        t.width = t.height = size;
        t.depthOrArray = 6;
        t.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        t.samples = 1;
        t.mips = mips;
        t.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!srv.ptr)
            srv = g_srvCpu.Alloc();
        D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vd.TextureCube.MipLevels = mips;
        g.dev->CreateShaderResourceView(t.res, &vd, srv);
        for (UINT m = 0; m < mips; m++)
        {
            if (!uav[m].ptr)
                uav[m] = g_srvCpu.Alloc();
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray = {m, 0, 6, 0};
            g.dev->CreateUnorderedAccessView(t.res, nullptr, &ud, uav[m]);
        }
        Log("engine cube for the materials: %ux%u, %u levels (GGX)", size, size, mips);
        return true;
    }

    // The materials' copies (every material, own maps) from the HDR copy: level m convolved with GGX of roughness m / 6
    // (alpha = (m / 6)^2, up to 1); the HDR copy is in kSrvState. ~0.1 ms GPU at 256.
    void EncodeMaterialsCube(UINT size, UINT mips)
    {
        if (!m_cubeEncodePso || !EnsureMaterialsCube(m_ldrCube, m_ldrCubeSrv, m_ldrCubeUav, size, mips) ||
            !EnsureMaterialsCube(m_ldrCubeOwn, m_ldrCubeOwnSrv, m_ldrCubeOwnUav, size, mips))
            return;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu0;
        while (!m_srvRing.TryAlloc(4 * mips, cpu0, gpu0))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        Require(&m_ldrCube, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Require(&m_ldrCubeOwn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FlushBarriers();
        const bool keys = g_giControl.active;
        const float scaleAll = keys ? g_giControl.envAllScale.load() : 1.0f, scaleOwn = keys ? g_giControl.envOwnScale.load() : 1.0f;
        m_list->SetComputeRootSignature(m_cubeRoot);
        m_list->SetPipelineState(m_cubeEncodePso);
        for (UINT m = 0; m < mips; m++)
        {
            const UINT dsize = std::max(1u, size >> m);
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu{cpu0.ptr + 4 * m * g.srvInc};
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu{gpu0.ptr + 4 * m * g.srvInc};
            g.dev->CopyDescriptorsSimple(1, cpu, g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + g.srvInc}, m_hdrCubeSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + 2 * g.srvInc}, m_ldrCubeUav[m], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g.dev->CopyDescriptorsSimple(1, {cpu.ptr + 3 * g.srvInc}, m_ldrCubeOwnUav[m], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const float r = std::min(m / 6.0f, 1.0f), alpha = r * r;
            uint32_t k[8] = {dsize, 0, 0, 0, 0, 0, 0, size};
            memcpy(&k[4], &alpha, sizeof(float));
            memcpy(&k[5], &scaleAll, sizeof(float));
            memcpy(&k[6], &scaleOwn, sizeof(float));
            m_list->SetComputeRoot32BitConstants(0, 8, k, 0);
            m_list->SetComputeRootDescriptorTable(1, gpu);
            m_list->Dispatch((dsize + 7) / 8, (dsize + 7) / 8, 6);
        }
        Require(&m_ldrCube, kSrvState);
        Require(&m_ldrCubeOwn, kSrvState);
        FlushBarriers();
        m_ldrCubeReady = true;
        RunEnv2D(mips);
    }

    // The weapons' 2D environment map from the materials' cube (kEnv2DShader), after each update of the cube.
    void RunEnv2D(UINT cubeMips)
    {
        if (!m_ldrCubeOwn.res || !m_ldrCubeOwnSrv.ptr)
            return;
        if (!m_env2DPso && !m_env2DFailed)
        {
            m_env2DFailed = true;
            using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
            HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
            auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            if (compile && g_serializeRoot &&
                SUCCEEDED(compile(kEnv2DShader, strlen(kEnv2DShader), "CryMT_Env2D", nullptr, nullptr, "env2d", "cs_5_0", 1 << 15, 0, &code, &errors)))
            {
                D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
                D3D12_ROOT_PARAMETER params[2]{};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[0].Constants.Num32BitValues = 2;
                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[1].DescriptorTable = {2, ranges};
                D3D12_STATIC_SAMPLER_DESC ss{};
                ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                ss.MaxLOD = D3D12_FLOAT32_MAX;
                ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                D3D12_ROOT_SIGNATURE_DESC rd{};
                rd.NumParameters = 2;
                rd.pParameters = params;
                rd.NumStaticSamplers = 1;
                rd.pStaticSamplers = &ss;
                ID3DBlob* blob = nullptr;
                HRESULT hr = g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
                if (SUCCEEDED(hr))
                    hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                                    reinterpret_cast<void**>(&m_env2DRoot));
                if (blob)
                    blob->Release();
                if (SUCCEEDED(hr))
                {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = m_env2DRoot;
                    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
                    hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&m_env2DPso));
                }
                m_env2DFailed = FAILED(hr);
                if (m_env2DFailed)
                {
                    m_env2DPso = nullptr;
                    Log("weapons' 2D environment map pipeline: 0x%08x", static_cast<unsigned>(hr));
                }
            }
            else
                Log("weapons' 2D environment map shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            if (code)
                code->Release();
            if (errors)
                errors->Release();
        }
        if (!m_env2DPso)
            return;
        constexpr UINT kSize = 64;
        if (!m_ldr2D.res)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = kSize;
            d.Height = kSize;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_ldr2D.res))))
            {
                m_ldr2D.res = nullptr;
                m_env2DPso = nullptr;
                m_env2DFailed = true;
                return;
            }
            m_ldr2D.dim = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            m_ldr2D.width = m_ldr2D.height = kSize;
            m_ldr2D.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_ldr2D.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            m_ldr2DSrv = g_srvCpu.Alloc();
            m_ldr2DUav = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_ldr2D.res, nullptr, m_ldr2DSrv);
            g.dev->CreateUnorderedAccessView(m_ldr2D.res, nullptr, nullptr, m_ldr2DUav);
            Log("weapons' 2D environment map from the engine cube: %ux%u", kSize, kSize);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(2, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        g.dev->CopyDescriptorsSimple(1, cpu, m_ldrCubeOwnSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g.dev->CopyDescriptorsSimple(1, {cpu.ptr + g.srvInc}, m_ldr2DUav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Require(&m_ldrCubeOwn, kSrvState);
        Require(&m_ldr2D, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FlushBarriers();
        // Level 3 of the materials' cube (GGX alpha 0.25): about the voxel 2D map's blur (its cones' half tangent 0.25).
        const float lod = static_cast<float>(std::min(3u, cubeMips ? cubeMips - 1 : 0u));
        uint32_t k[2] = {kSize, 0};
        memcpy(&k[1], &lod, sizeof(float));
        m_list->SetComputeRootSignature(m_env2DRoot);
        m_list->SetPipelineState(m_env2DPso);
        m_list->SetComputeRoot32BitConstants(0, 2, k, 0);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->Dispatch((kSize + 7) / 8, (kSize + 7) / 8, 1);
        Require(&m_ldr2D, kSrvState);
        FlushBarriers();
        m_ldr2DReady = true;
    }

    bool PsReads(const ResourceInfo* placeholder) const
    {
        const StageState& st = m_stage[kPS];
        for (UINT i = 0; i < m_ps->srvEnd && i < kTableSrvs; i++)
            if (st.srv[i] && st.srv[i]->info == placeholder)
                return true;
        return false;
    }

    bool PsUsesGiPlaceholder() const
    {
        const StageState& st = m_stage[kPS];
        for (UINT i = 0; i < m_ps->srvEnd && i < kTableSrvs; i++)
            if (st.srv[i] && st.srv[i]->info == g_giPlaceholder)
                return true;
        return false;
    }

    static DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return f;
        }
    }

    void RunGi()
    {
        m_giLastRun = m_giFrame;
        m_giRunFrame = m_frame;
        m_note.gi = 1;
        if (!m_dsv || !m_giHaveCamera || !EnsureGiPipeline())
            return;
        TextureCore* depth = SceneDepth(static_cast<TextureCore*>(m_dsv->info), m_giFullW, m_giFullH);
        if (!depth)
            return;
        const int prev = m_giCur, cur = m_giCur ^ 1;
        Require(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_barriers.push_back(Transition(m_giHist[cur], m_giHistState[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        const bool cones = Gi().voxelMix > 0 && Gi().voxelMix < 4 && m_voxValid; // 4: the grid only (cost)
        if (cones)
            for (int l = 0; l < kVoxLevels; l++)
            {
                m_barriers.push_back(Transition(m_vox[l], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                m_barriers.push_back(Transition(m_voxDir[l], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                m_barriers.push_back(Transition(m_voxL1[l], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                for (int f = 0; f < 6 && m_voxSixReady; f++)
                    m_barriers.push_back(Transition(m_voxSix[l][f], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            }
        if (cones)
            m_barriers.push_back(Transition(m_voxRoof, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        // GI v2: this frame's probe atlases written (u0 light and states, u1 distances), last frame's read (t2, t1).
        const bool probesOn = cones && Gi().probes > 0 && m_giPsoProbes && EnsureProbes();
        ID3D12Resource* const probeCur = probesOn ? m_probeAtlas[m_probeCur] : nullptr;
        ID3D12Resource* const probePrev = probesOn ? m_probeAtlas[m_probeCur ^ 1] : nullptr;
        ID3D12Resource* const probeDepthCur = probesOn ? m_probeDepth[m_probeCur] : nullptr;
        ID3D12Resource* const probeDepthPrev = probesOn ? m_probeDepth[m_probeCur ^ 1] : nullptr;
        if (probesOn)
            for (int i = 0; i < 2; i++)
            {
                const D3D12_RESOURCE_STATES want = i == m_probeCur ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                if (m_probeAtlasState[i] != want)
                {
                    m_barriers.push_back(Transition(m_probeAtlas[i], m_probeAtlasState[i], want));
                    m_barriers.push_back(Transition(m_probeDepth[i], m_probeAtlasState[i], want));
                }
                m_probeAtlasState[i] = want;
            }
        FlushBarriers();
        // The VisAreas near the camera for the trace (t11, VisInside): uploaded before the descriptors and any state.
        constexpr UINT kVisFloat4s = 1 + 16 * 10 + 1; // the header, 16 areas, GI v2's options (kGiShader kProbeOptions)
        const UploadAlloc visUp = FrameUpload(kVisFloat4s * 16, 256);
        if (visUp.cpu)
        {
            auto* f = reinterpret_cast<float*>(visUp.cpu);
            memset(f, 0, kVisFloat4s * 16);
            f[0] = static_cast<float>(m_giVisAreaCount);
            f[1] = Gi().roomLight ? 1.0f : 0.0f; // [0].y: the cones from inside a room take no light from outside its box
            f[2] = static_cast<float>(ConeDirMode()); // [0].z: the cells' light, 0 one colour, 1 L1, 2 six directions
            // [0].w: GI v2 on, its bounce light's factor, + 100 when it gives the light only (method 2: the cones keep the
            // occlusion); negative without the visibility test (CRYMT_GI_PROBEVIS=0)
            f[3] = probesOn ? (std::max(Gi().probeStrength, 1e-4f) + (Gi().probes == 2 ? 100.0f : 0.0f)) * (Gi().probeVisibility ? 1.0f : -1.0f)
                            : 0.0f;
            for (uint32_t k = 0; k < m_giVisAreaCount; k++)
                memcpy(f + 4 + k * 40, m_giVisAreas[k], sizeof(m_giVisAreas[k]));
            // [161].x..z: free (precise rays, canopy, foliage smoothing removed 2026-11-15)
            f[(kVisFloat4s - 1) * 4 + 3] = Gi().gtaoHistoryWeight;          // [161].w: GTAO's history weight (gtaoBlur)
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        // Three tables of 6: trace (depth, colour, -, raw out, -, -), filter (depth, raw, previous GI, new GI out,
        // distance out, previous distance) and upsample (depth, new GI, -, full-size out, distances, -).
        const UINT aoTables = 24 + kGiVoxTable + 1; // (+1: u3 probeMatch) then three tables of 6 for the GTAO passes, the particles' volume, the
                                                // environment maps and GI v2's probes (aoTables + 30)
        while (!m_srvRing.TryAlloc(aoTables + 36, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DepthSrvFormat(depth->format);
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(depth->res, &sd, at(0));
        g.dev->CreateShaderResourceView(m_giColor, nullptr, at(1));
        g.dev->CreateShaderResourceView(m_giHist[prev], nullptr, at(2));
        g.dev->CreateUnorderedAccessView(m_giRaw, nullptr, nullptr, at(3));
        g.dev->CreateUnorderedAccessView(m_giDist[cur], nullptr, nullptr, at(4));
        g.dev->CreateUnorderedAccessView(m_giDist[prev], nullptr, nullptr, at(5));
        g.dev->CreateShaderResourceView(depth->res, &sd, at(6));
        g.dev->CreateShaderResourceView(m_giRaw, nullptr, at(7));
        g.dev->CreateShaderResourceView(m_giHist[prev], nullptr, at(8));
        g.dev->CreateUnorderedAccessView(m_giHist[cur], nullptr, nullptr, at(9));
        g.dev->CreateUnorderedAccessView(m_giDist[cur], nullptr, nullptr, at(10));
        g.dev->CreateUnorderedAccessView(m_giDist[prev], nullptr, nullptr, at(11));
        g.dev->CreateShaderResourceView(depth->res, &sd, at(12));
        g.dev->CreateShaderResourceView(m_giHist[cur], nullptr, at(13));
        g.dev->CreateShaderResourceView(m_giCam ? m_giCam : m_giHist[cur], nullptr, at(14)); // t2: the camera's GI
        g.dev->CreateUnorderedAccessView(m_giFull, nullptr, nullptr, at(15));
        g.dev->CreateUnorderedAccessView(m_giDist[cur], nullptr, nullptr, at(16));
        g.dev->CreateUnorderedAccessView(m_giDist[prev], nullptr, nullptr, at(17));
        // Fourth table: the camera pass (u0 its 1x1 texture; the rest as the trace's).
        g.dev->CreateShaderResourceView(depth->res, &sd, at(18));
        g.dev->CreateShaderResourceView(m_giColor, nullptr, at(19));
        g.dev->CreateShaderResourceView(m_giHist[prev], nullptr, at(20));
        g.dev->CreateUnorderedAccessView(m_giCam ? m_giCam : m_giRaw, nullptr, nullptr, at(21));
        g.dev->CreateUnorderedAccessView(m_giDist[cur], nullptr, nullptr, at(22));
        g.dev->CreateUnorderedAccessView(m_giDist[prev], nullptr, nullptr, at(23));
        for (int l = 0; l < kVoxLevels; l++)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            vd.Texture3D.MipLevels = cones ? kVoxMips : 1;
            g.dev->CreateShaderResourceView(cones ? m_vox[l] : nullptr, &vd, at(24 + l));
            vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            vd.Texture3D.MipLevels = cones ? kVoxMips - 1 : 1;
            g.dev->CreateShaderResourceView(cones ? m_voxDir[l] : nullptr, &vd, at(24 + kVoxLevels + l));
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC bd{};
        bd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        bd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        bd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        bd.Buffer.FirstElement = visUp.cpu ? visUp.offset / 16 : 0;
        bd.Buffer.NumElements = kVisFloat4s;
        g.dev->CreateShaderResourceView(visUp.cpu ? visUp.res : nullptr, &bd, at(24 + 2 * kVoxLevels));
        D3D12_SHADER_RESOURCE_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R8_UNORM;
        rv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        rv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rv.Texture3D.MipLevels = 1;
        g.dev->CreateShaderResourceView(cones ? m_voxRoof : nullptr, &rv, at(24 + 2 * kVoxLevels + 1));
        for (int l = 0; l < kVoxLevels; l++) // t13..t16: the levels' light direction (CRYMT_GI_DIRLIGHT 1)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC ld{};
            ld.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
            ld.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            ld.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ld.Texture3D.MipLevels = cones ? kVoxMips : 1;
            g.dev->CreateShaderResourceView(cones ? m_voxL1[l] : nullptr, &ld, at(24 + 2 * kVoxLevels + 2 + l));
            ld.Format = DXGI_FORMAT_R11G11B10_FLOAT; // t17..t40: the six directions, level-major (CRYMT_GI_DIRLIGHT 2)
            ld.Texture3D.MipLevels = cones && m_voxSixReady ? kVoxMips : 1;
            for (int f = 0; f < 6; f++)
                g.dev->CreateShaderResourceView(cones && m_voxSixReady ? m_voxSix[l][f] : nullptr, &ld, at(24 + 3 * kVoxLevels + 2 + l * 6 + f));
        }
        {
            // t41, t42: GI v2's atlases written this frame (read by the trace); the probes' table: (depth, last frame's
            // distances, last frame's light, this frame's light out, distances out, -).
            D3D12_SHADER_RESOURCE_VIEW_DESC pd{};
            pd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            pd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            pd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            pd.Texture2D.MipLevels = 1;
            D3D12_SHADER_RESOURCE_VIEW_DESC pdd = pd;
            pdd.Format = DXGI_FORMAT_R16G16_FLOAT;
            g.dev->CreateShaderResourceView(probeCur, &pd, at(24 + kGiVoxTable - 3));
            g.dev->CreateShaderResourceView(probeDepthCur, &pdd, at(24 + kGiVoxTable - 2));
            // t43: the foliage mask for the upsample's smoothing on grass and leaves (null: none)
            const bool foliageOk = false && m_foliageSrv && m_foliageFrame == m_frame && m_foliageW == m_giFullW && m_foliageH == m_giFullH;
            if (foliageOk)
            {
                m_foliageSrv->Refresh();
                Require(m_foliageSrv->info, kSrvState);
            }
            g.dev->CopyDescriptorsSimple(1, at(24 + kGiVoxTable - 1), foliageOk ? m_foliageSrv->cpu : g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            // u3 after the second table's textures: probeMatch (a null view with probes off)
            D3D12_UNORDERED_ACCESS_VIEW_DESC md{};
            md.Format = DXGI_FORMAT_R32_UINT;
            md.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            md.Buffer.NumElements = 16;
            g.dev->CreateUnorderedAccessView(probesOn ? m_probeMatch : nullptr, nullptr, &md, at(24 + kGiVoxTable));
            const int bp = static_cast<int>(aoTables) + 30;
            g.dev->CreateShaderResourceView(depth->res, &sd, at(bp));
            g.dev->CreateShaderResourceView(probeDepthPrev, &pdd, at(bp + 1));
            g.dev->CreateShaderResourceView(probePrev, &pd, at(bp + 2));
            g.dev->CreateUnorderedAccessView(probeCur ? probeCur : m_giRaw, nullptr, nullptr, at(bp + 3));
            g.dev->CreateUnorderedAccessView(probeDepthCur ? probeDepthCur : m_giRaw, nullptr, nullptr, at(bp + 4));
            g.dev->CreateUnorderedAccessView(m_giDist[prev], nullptr, nullptr, at(bp + 5));
        }
        // GTAO: (depth, foliage mask, -, -, raw out, -), (depth, raw, previous history, current history out, -, -),
        // (depth, current history, -, full-size GI, -, -).
        const int aoCur = m_aoCur ^ 1, aoPrev = m_aoCur;
        {
            const int b0 = static_cast<int>(aoTables);
            for (int i = 0; i < 3; i++)
                g.dev->CreateShaderResourceView(depth->res, &sd, at(b0 + i));
            if (m_foliageSrv && m_foliageW == m_giFullW && m_foliageH == m_giFullH)
                g.dev->CreateShaderResourceView(m_foliageSrv->info->res, nullptr, at(b0 + 1));
            else
                NullSrv(at(b0 + 1));
            const auto* mask = m_shadowMaskRtv ? static_cast<const TextureCore*>(m_shadowMaskRtv->info) : nullptr;
            if (mask && mask->width == m_giFullW && mask->height == m_giFullH)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC md{};
                md.Format = m_shadowMaskRtv->format;
                md.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                md.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                md.Texture2D.MipLevels = 1;
                g.dev->CreateShaderResourceView(m_shadowMaskRtv->info->res, &md, at(b0 + 2));
            }
            else
                NullSrv(at(b0 + 2));
            for (int i = 3; i < 6; i++)
                g.dev->CreateUnorderedAccessView(m_aoRaw, nullptr, nullptr, at(b0 + i));
            const int b1 = b0 + 6;
            g.dev->CreateShaderResourceView(depth->res, &sd, at(b1));
            g.dev->CreateShaderResourceView(m_aoRaw, nullptr, at(b1 + 1));
            g.dev->CreateShaderResourceView(m_aoHist[aoPrev], nullptr, at(b1 + 2));
            for (int i = 3; i < 6; i++)
                g.dev->CreateUnorderedAccessView(m_aoHist[aoCur], nullptr, nullptr, at(b1 + i));
            const int b2 = b0 + 12;
            g.dev->CreateShaderResourceView(depth->res, &sd, at(b2));
            g.dev->CreateShaderResourceView(m_aoHist[aoCur], nullptr, at(b2 + 1));
            if (m_foliageSrv && m_foliageW == m_giFullW && m_foliageH == m_giFullH)
                g.dev->CreateShaderResourceView(m_foliageSrv->info->res, nullptr, at(b2 + 2)); // the foliage mask
            else
                NullSrv(at(b2 + 2));
            for (int i = 3; i < 6; i++)
                g.dev->CreateUnorderedAccessView(m_giFull, nullptr, nullptr, at(b2 + i));
            // The particles' occlusion volume: (depth, -, -, atlas out, -, -).
            const int b3 = b0 + 18;
            for (int i = 0; i < 3; i++)
                g.dev->CreateShaderResourceView(depth->res, &sd, at(b3 + i));
            for (int i = 3; i < 6; i++)
                if (m_pvol)
                    g.dev->CreateUnorderedAccessView(m_pvol, nullptr, nullptr, at(b3 + i));
                else
                    g.dev->CreateUnorderedAccessView(m_giFull, nullptr, nullptr, at(b3 + i));
            // The environment maps: (depth, the previous frame's colour, -, atlas out, -, -).
            const int b4 = b0 + 24;
            g.dev->CreateShaderResourceView(depth->res, &sd, at(b4));
            g.dev->CreateShaderResourceView(m_giColor, nullptr, at(b4 + 1));
            const bool env = EnvOn() && EnsureEnvTargets();
            g.dev->CreateShaderResourceView(env ? m_envAtlas[m_envCur ^ 1] : m_giHist[prev], nullptr, at(b4 + 2));
            for (int i = 3; i < 6; i++)
                g.dev->CreateUnorderedAccessView(env ? m_envAtlas[m_envCur] : m_giFull, nullptr, nullptr, at(b4 + i));
        }
        // History reprojection: this view to the world, then the previous view and projection. A camera cut has none.
        float toWorld[16], reproj[16];
        RigidInverse(m_giView, toWorld);
        if (CameraJump(m_giView, m_giCamPos, m_giPrevView, m_giPrevCamPos) > 10.0)
            m_giReset = true;
        if (m_giReset)
        {
            memcpy(m_giPrevView, m_giView, sizeof(m_giPrevView));
            memcpy(m_giPrevProj, m_giProj, sizeof(m_giPrevProj));
            memcpy(m_giPrevCamPos, m_giCamPos, sizeof(m_giPrevCamPos));
        }
        ReprojDouble(m_giView, m_giCamPos, m_giPrevView, m_giPrevCamPos, m_giPrevProj, reproj);
        const GiSettings& s = Gi();
        float c[kGiConstants] = {static_cast<float>(m_giFullW), static_cast<float>(m_giFullH), static_cast<float>(m_giW), static_cast<float>(m_giH),
                                 1.0f / m_giProj[0], 1.0f / m_giProj[5], m_giProj[10], m_giProj[14],
                                 0.0f, 0.0f, 0.0f, m_giReset ? 0.0f : s.temporal,
                                 s.maxDistance, static_cast<float>(m_giFrame & 1023), s.debug, (s.fixedNoise ? 1.0f : 0.0f) + (s.nearest ? 2.0f : 0.0f) + (s.narrowNormals ? 8.0f : 0.0f) +
                                 (cones && s.weaponGi && m_giCam && m_giPsoCamera ? 16.0f : 0.0f) +
                                 (s.mipOpacity >= 4 ? 64.0f : 0.0f) + (s.roofCells ? 128.0f : 0.0f) + (s.thinFix ? 256.0f : 0.0f) +
                                 (s.replaceSsao ? 512.0f : 0.0f) + static_cast<float>((s.bakedOff & 30) << 10) + (s.gtaoFull ? 32768.0f : 0.0f) + (s.gtaoHistory ? 1024.0f : 0.0f) + (s.gtaoNoFoliage ? 4194304.0f : 0.0f) +
                                 (s.gtaoFoliageShade ? 8388608.0f : 0.0f) +
                                 static_cast<float>(std::min(31, std::max(0, static_cast<int>(s.terrainAoNear * 0.5f + 0.5f))) * 65536) +
                                 (s.particlesGi ? 2097152.0f : 0.0f)};  // + reproj, motionParams 0
        memcpy(c + 16, reproj, sizeof(reproj));
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 4; i++)
                c[36 + j * 4 + i] = toWorld[i * 4 + j];
        const float voxel[8] = {static_cast<float>(m_voxGrid[0][0]), static_cast<float>(m_voxGrid[0][1]), static_cast<float>(m_voxGrid[0][2]),
                                static_cast<float>(kVoxN), s.voxelSize, cones ? s.voxelMix : 0.0f, s.voxelReach, s.voxelStrength};
        memcpy(c + 48, voxel, sizeof(voxel));
        c[56] = s.voxelOcclusion;
        c[57] = s.coneLift;
        c[58] = s.coneStart;
        c[59] = s.nearOcclusion;
        c[60] = s.occFar;
        c[32] = s.gtaoRadius;      // motionParams.x: GTAO radius (m)
        c[33] = s.gtao;            // motionParams.y: GTAO strength (0 off)
        c[34] = s.ambientFloor;    // motionParams.z
        c[35] = s.outdoorStrength; // motionParams.w
        c[61] = s.occStrength;

        memcpy(m_giPrevView, m_giView, sizeof(m_giPrevView));
        memcpy(m_giPrevProj, m_giProj, sizeof(m_giPrevProj));
        memcpy(m_giPrevCamPos, m_giCamPos, sizeof(m_giPrevCamPos));
        memcpy(m_giPrevToWorld, toWorld, sizeof(m_giPrevToWorld));
        const UINT groupsX = (m_giW + 7) / 8, groupsY = (m_giH + 7) / 8;
        m_list->SetComputeRootSignature(m_giRoot);
        m_list->SetComputeRoot32BitConstants(0, kGiConstants, c, 0);
        m_list->SetPipelineState(m_giPso);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->SetComputeRootDescriptorTable(2, {gpu.ptr + 24 * g.srvInc});
        // The slot's last run is long done: its times into the statistics, then this run's stamps.
        if (m_giStamps && m_giStampRun >= kGiStampRuns)
        {
            const uint64_t* t = m_giStamps + (m_giStampRun % kGiStampRuns) * kGiStamps;
            bool ordered = true;
            for (UINT i = 1; i < kGiStamps; i++)
                ordered = ordered && t[i] >= t[i - 1];
            if (ordered && t[kGiStamps - 1] > t[0])
            {
                for (UINT i = 0; i + 1 < kGiStamps; i++)
                    m_stats.giStampTicks[i] += t[i + 1] - t[i];
                m_stats.giStampRuns++;
            }
        }
        GiStamp(0);
        D3D12_RESOURCE_BARRIER distances{};
        distances.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // last frame's distance writes before this frame's reads
        m_list->ResourceBarrier(1, &distances);
        if (probesOn)
        {
            // GI v2: a group a probe slot (128 x 128 = kProbeN's 16384), then the atlas for the trace to read (t41).
            PassName("CryMT GI probes"); // its own GPU time in the census (CRYMT_GPUPASSES)
            m_list->SetPipelineState(m_giPsoProbes);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + (aoTables + 30) * g.srvInc});
            m_list->Dispatch(128, 128, kProbeCascades); // z: the cascade
            PassNameEnd();
            D3D12_RESOURCE_BARRIER toRead[3] = {Transition(probeCur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                                Transition(probeDepthCur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                                {}};
            toRead[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // probeMatch: the ratio set and the sums cleared before the trace
            toRead[2].UAV.pResource = m_probeMatch;
            m_list->ResourceBarrier(3, toRead);
            if (m_giDumpWanted) // crymt_capture: both atlases as they are (<tag>_probes.raw, <tag>_probedist.raw)
            {
                D3D12_RESOURCE_BARRIER toCopy[2] = {Transition(probeCur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                    Transition(probeDepthCur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
                m_list->ResourceBarrier(2, toCopy);
                RecordDump(probeCur, "probes", 11);
                RecordDump(probeDepthCur, "probedist", 11);
                for (D3D12_RESOURCE_BARRIER& b : toCopy)
                    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                m_list->ResourceBarrier(2, toCopy);
            }
            m_probeAtlasState[m_probeCur] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            m_probeCur ^= 1;
            m_stats.giProbeRuns++;
            m_list->SetPipelineState(m_giPso);
            m_list->SetComputeRootDescriptorTable(1, gpu);
        }
        m_list->Dispatch(groupsX, groupsY, 1);
        const bool weapon = cones && s.weaponGi && m_giCam && m_giPsoCamera;
        if (weapon)
        {
            m_list->SetPipelineState(m_giPsoCamera);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + 18 * g.srvInc});
            m_list->Dispatch(1, 1, 1);
        }
        GiStamp(1);
        // Environment maps from the grid, every 4th frame: the atlas into the cube's faces and the 2D map.
        if (cones && EnvOn() && m_envPso && m_envAtlas[0] && (m_giFrame & 3) == 0)
        {
            ID3D12Resource* cur = m_envAtlas[m_envCur];
            ID3D12Resource* other = m_envAtlas[m_envCur ^ 1];
            m_list->SetPipelineState(m_envPso);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + (aoTables + 24) * g.srvInc});
            const bool keys = g_giControl.active;
            const float scales[2] = {keys ? g_giControl.envOwnScale.load() : 1.0f, keys ? g_giControl.envAllScale.load() : 1.0f};
            m_list->SetComputeRoot32BitConstants(0, 2, scales, 16);
            m_list->Dispatch(kEnvSize * 13 / 8, kEnvSize / 8, 1);
            m_list->SetComputeRoot32BitConstants(0, 2, c + 16, 16); // the reprojection the later passes read
            D3D12_RESOURCE_BARRIER toCopy = Transition(cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_list->ResourceBarrier(1, &toCopy);
            Require(m_envCube->info, D3D12_RESOURCE_STATE_COPY_DEST);
            Require(m_env2D->info, D3D12_RESOURCE_STATE_COPY_DEST);
            Require(m_envCubeAll->info, D3D12_RESOURCE_STATE_COPY_DEST);
            FlushBarriers();
            // Squares 13..18 the materials' cube, 19 their 2D map, 20..25 the cube for every material (each with its
            // strength; 0..12 are the pass's history).
            for (UINT f = 0; f < 13; f++)
            {
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = f < 6 ? m_envCube->info->res : f == 6 ? m_env2D->info->res : m_envCubeAll->info->res;
                dst.Type = src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = f < 6 ? f : f == 6 ? 0 : f - 7; // one mip
                src.pResource = cur;
                const UINT sq = 13 + f;
                const D3D12_BOX box{sq * kEnvSize, 0, 0, (sq + 1) * kEnvSize, kEnvSize, 1};
                NoteCopy("voxel view", dst, 0, 0, 0, src, &box);
                m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            }
            // This run's atlas is read by the next; the previous one is written then.
            D3D12_RESOURCE_BARRIER swap[2] = {Transition(cur, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                              Transition(other, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            m_list->ResourceBarrier(2, swap);
            m_envCur ^= 1;
            m_envReady = true;
        }
        D3D12_RESOURCE_BARRIER rawRead = Transition(m_giRaw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_list->ResourceBarrier(1, &rawRead);
        m_list->SetPipelineState(m_giPsoFilter);
        m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + 6 * g.srvInc});
        m_list->Dispatch(groupsX, groupsY, 1);
        GiStamp(2);
        if (m_giDumpWanted)
        {
            D3D12_RESOURCE_BARRIER b = Transition(m_giRaw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_list->ResourceBarrier(1, &b);
            RecordDump(m_giRaw, "raw", 2);
            m_barriers.push_back(Transition(m_giRaw, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        }
        else
            m_barriers.push_back(Transition(m_giRaw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        if (s.upsample && m_giPsoUpsample)
        {
            // The filter's GI and distances, read by the full-size pass.
            D3D12_RESOURCE_BARRIER b[3] = {Transition(m_giHist[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrvState),
                                           Transition(m_giFull, kSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), {}};
            b[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            m_list->ResourceBarrier(3, b);
            if (m_giCam)
            {
                D3D12_RESOURCE_BARRIER cam = Transition(m_giCam, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                m_list->ResourceBarrier(1, &cam);
            }
            FlushBarriers(); // the foliage mask (t43) readable
            m_list->SetPipelineState(m_giPsoUpsample);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + 12 * g.srvInc});
            m_list->Dispatch((m_giFullW + 7) / 8, (m_giFullH + 7) / 8, 1);
            if (s.replaceSsao && s.gtao > 0 && m_aoPso && m_aoBlurPso && m_aoApplyPso && m_aoRaw && m_aoHist[0] && m_aoHist[1])
            {
                // GTAO: half-size occlusion; its blur blended with the previous frame's (the history, reprojected);
                // then the factor on the full-size GI just written.
                const UINT aoScale = s.gtaoFull ? 1 : 2;
                const UINT aoW = (m_giFullW + aoScale - 1) / aoScale, aoH = (m_giFullH + aoScale - 1) / aoScale;
                if (m_foliageSrv && m_foliageW == m_giFullW && m_foliageH == m_giFullH)
                    Require(m_foliageSrv->info, kSrvState);
                if (m_shadowMaskRtv && static_cast<const TextureCore*>(m_shadowMaskRtv->info)->width == m_giFullW &&
                    static_cast<const TextureCore*>(m_shadowMaskRtv->info)->height == m_giFullH)
                    Require(m_shadowMaskRtv->info, kSrvState);
                FlushBarriers();
                std::vector<D3D12_RESOURCE_BARRIER> pre;
                if (m_aoHistState[aoCur] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    pre.push_back(Transition(m_aoHist[aoCur], m_aoHistState[aoCur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                if (m_aoHistState[aoPrev] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    pre.push_back(Transition(m_aoHist[aoPrev], m_aoHistState[aoPrev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                if (!pre.empty())
                    m_list->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
                m_aoHistState[aoCur] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                m_aoHistState[aoPrev] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                m_list->SetPipelineState(m_aoPso);
                m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + aoTables * g.srvInc});
                m_list->Dispatch((aoW + 7) / 8, (aoH + 7) / 8, 1);
                D3D12_RESOURCE_BARRIER rawToRead = Transition(m_aoRaw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                m_list->ResourceBarrier(1, &rawToRead);
                m_list->SetPipelineState(m_aoBlurPso);
                m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + (aoTables + 6) * g.srvInc});
                m_list->Dispatch((aoW + 7) / 8, (aoH + 7) / 8, 1);
                D3D12_RESOURCE_BARRIER apply[2] = {Transition(m_aoHist[aoCur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), {}};
                apply[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                apply[1].UAV.pResource = m_giFull;
                m_list->ResourceBarrier(2, apply);
                m_aoHistState[aoCur] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                m_list->SetPipelineState(m_aoApplyPso);
                m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + (aoTables + 12) * g.srvInc});
                // motionParams.z for this pass: the leaves' factor (< 4) + 4 x floor(the grass's x 100)
                const float foliageGi = std::min(s.foliageGi, 3.99f) + 4.0f * std::floor(std::min(std::max(s.grassGi, 0.0f), 10.0f) * 100.0f + 0.5f) +
                                        4096.0f * std::floor(std::min(std::max(s.foliageBounce, 0.0f), 3.0f) * 100.0f + 0.5f);
                m_list->SetComputeRoot32BitConstants(0, 1, &foliageGi, 34);
                m_list->Dispatch((m_giFullW + 7) / 8, (m_giFullH + 7) / 8, 1);
                m_barriers.push_back(Transition(m_aoRaw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                m_aoCur = aoCur;
            }
            if (m_giDumpWanted)
            {
                if (m_shadowMaskRtv)
                {
                    Require(m_shadowMaskRtv->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    FlushBarriers();
                    RecordDump(m_shadowMaskRtv->info->res, "shadowmask", 5);
                }
                D3D12_RESOURCE_BARRIER b = Transition(m_giFull, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_list->ResourceBarrier(1, &b);
                RecordDump(m_giFull, "gi", 1);
                m_barriers.push_back(Transition(m_giFull, D3D12_RESOURCE_STATE_COPY_SOURCE, kSrvState));
                // This frame's distances at the GI's size (<tag>_dist.r32): the GI by distance (tools\prof\gi_by_distance.py).
                b = Transition(m_giDist[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_list->ResourceBarrier(1, &b);
                RecordDump(m_giDist[cur], "dist", 10);
                b = Transition(m_giDist[cur], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                m_list->ResourceBarrier(1, &b);
            }
            else
                m_barriers.push_back(Transition(m_giFull, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrvState));
            m_giDumpWanted = false;
            if (m_giCam)
                m_barriers.push_back(Transition(m_giCam, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            m_giCpu = m_giFullSrv;
        }
        else
        {
            m_barriers.push_back(Transition(m_giHist[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrvState));
            m_giCpu = m_giHistSrv[cur];
        }
        m_giHistState[cur] = kSrvState;
        m_giCur = cur;
        GiStamp(3);
        if (cones && (s.particlesGi || s.vfog) && m_pvol && m_pvolPso)
        {
            // The particles' occlusion volume around the camera, from the grid (its parameters in reproj's rows); also
            // for the GI in the fog (RunVfog), with the particles' own GI off.
            FlushBarriers();
            float pc[kGiConstants];
            memcpy(pc, c, sizeof(pc));
            pc[16] = s.particleCell;
            pc[17] = static_cast<float>(kPvolX);
            pc[18] = static_cast<float>(kPvolY);
            pc[19] = static_cast<float>(kPvolZ);
            pc[20] = static_cast<float>(kPvolTiles);
            pc[21] = s.particleFineCell;
            pc[22] = static_cast<float>(m_giFrame & 1); // the fine volume one frame, the coarse the next
            pc[24] = m_giSun[0];
            pc[25] = m_giSun[1];
            pc[26] = m_giSun[2];
            pc[27] = m_giSun[3] > 0 ? 1.0f : 0.0f; // towards the sun, found
            pc[28] = s.particleOutdoor;
            pc[29] = s.particleMax;
            pc[30] = s.particleTint;
            D3D12_RESOURCE_BARRIER b = Transition(m_pvol, kSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_list->ResourceBarrier(1, &b);
            m_list->SetComputeRoot32BitConstants(0, kGiConstants, pc, 0);
            m_list->SetPipelineState(m_pvolPso);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + (aoTables + 18) * g.srvInc});
            m_list->Dispatch((kPvolX * kPvolTiles + 7) / 8, (kPvolY * ((kPvolZ + kPvolTiles - 1) / kPvolTiles) + 7) / 8, 1);
            m_barriers.push_back(Transition(m_pvol, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrvState));
            m_pvolBuilt = true;
            m_pvolFrame = m_frame;
        }
        if (cones)
            for (int l = 0; l < kVoxLevels; l++)
            {
                m_barriers.push_back(Transition(m_vox[l], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                m_barriers.push_back(Transition(m_voxDir[l], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                m_barriers.push_back(Transition(m_voxL1[l], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                for (int f = 0; f < 6 && m_voxSixReady; f++)
                    m_barriers.push_back(Transition(m_voxSix[l][f], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            }
        if (cones)
            m_barriers.push_back(Transition(m_voxRoof, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        if (s.voxels)
        {
            PassName("CryMT GI voxels");
            RunVoxelUpdate(depth, sd, toWorld, reproj);
            PassNameEnd();
        }
        else
            GiStamp(4);
        // Where the ocean comes in the z-prepass (decides how the voxels' depth can leave it out), now and then.
        if (m_firstWaterZ > 0 && (m_giFrame % 600) == 0)
            Log("z-prepass at frame %llu: %d draws recorded, the ocean's first is #%d", static_cast<unsigned long long>(m_giFrame), m_zDraws, m_firstWaterZ);
        m_zDraws = 0;
        m_firstWaterZ = -1;
        GiStamp(5);
        if (m_giStamps)
        {
            const UINT first = static_cast<UINT>(m_giStampRun % kGiStampRuns) * kGiStamps;
            m_list->ResolveQueryData(m_giStampHeap, D3D12_QUERY_TYPE_TIMESTAMP, first, kGiStamps, m_giStampReadback, first * sizeof(uint64_t));
            m_giStampRun++;
        }
        m_giReset = false;
        m_stats.giPasses++;
        RestoreListState();
    }

    // Voxel grid (kVoxShader): pipeline and the grid texture on first use.
    bool EnsureVox()
    {
        if (m_voxFailed)
            return false;
        if (m_vox[0])
            return true;
        m_voxFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
            return false;
        ID3DBlob* code[6] = {};
        ID3DBlob* errors = nullptr;
        const char* entries[6] = {"update", "view", "downsample", "light", "roof", "inject"};
        for (int i = 0; i < 6; i++)
            if (FAILED(compile(kVoxShader, strlen(kVoxShader), "CryMT_Vox", nullptr, nullptr, entries[i], "cs_5_0", 1 << 15, 0, &code[i], &errors)))
            {
                Log("voxel shader %s: %s", entries[i], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
                return false;
            }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 20, 0, 0, 3}};
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = kVoxConstants;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b1: the lamps (light)
        params[2].Descriptor.ShaderRegister = 1;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b2: voxFlags (VoxFlags())
        params[3].Constants.ShaderRegister = 2;
        params[3].Constants.Num32BitValues = 1;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (int i = 0; i < 2; i++)
        {
            samplers[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
        }
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 4;
        rd.pParameters = params;
        rd.NumStaticSamplers = 2;
        rd.pStaticSamplers = samplers;
        ID3DBlob* blob = nullptr;
        if (FAILED(g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&m_voxRoot))))
        {
            Log("voxel root signature failed");
            return false;
        }
        blob->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_voxRoot;
        HRESULT hr = S_OK;
        ID3D12PipelineState** psos[6] = {&m_voxPsoUpdate, &m_voxPsoView, &m_voxPsoDown, &m_voxPsoLight, &m_voxPsoRoof, &m_voxPsoInject};
        for (int i = 0; i < 6; i++)
        {
            pd.CS = {code[i]->GetBufferPointer(), code[i]->GetBufferSize()};
            if (SUCCEEDED(hr))
                hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(psos[i]));
            code[i]->Release();
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        d.Width = d.Height = kVoxN;
        d.DepthOrArraySize = kVoxN;
        d.MipLevels = kVoxMips;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_vox[l]));
        // The cells' direction of light (CRYMT_GI_DIRLIGHT 1, VoxFlags): RGBA8 SNORM, xyz g with |g| <= 1, the light going
        // out towards w is the colour times 1 + g.w; the same mips as the grid. ~37 MB for four levels.
        d.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxL1[l]));
        d.Width = d.Height = kVoxN / 2;
        d.DepthOrArraySize = kVoxN / 2;
        d.MipLevels = kVoxMips - 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxDir[l]));
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(hr))
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxRoof));
        Log("voxel grid %d levels of %d^3 (%.2f m cells and up): %s", kVoxLevels, kVoxN, Gi().voxelSize, SUCCEEDED(hr) ? "ready" : "failed");
        for (auto& level : m_voxPrevGrid)
            for (int& v : level)
                v = -(1 << 28); // the first update resets every cell
        m_voxFailed = FAILED(hr);
        return SUCCEEDED(hr);
    }

    // kVoxTable descriptors (t0..t2, u0..u4) per table, count tables, on the shader-visible ring.
    void VoxTable(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu, UINT count = 1)
    {
        while (!m_srvRing.TryAlloc(kVoxTable * count, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
    }

    // A UAV of one mip of a grid level, or a null 3D view.
    void VoxUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 ? nullptr : m_vox[level], nullptr, &ud, at);
    }

    // A UAV of one mip of a level's light direction (m_voxL1), or a null 3D view.
    void L1Uav(D3D12_CPU_DESCRIPTOR_HANDLE at, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 || level < 0 ? nullptr : m_voxL1[level], nullptr, &ud, at);
    }

    // The voxel passes' b2 (voxFlags): bit 0 the cells' light direction (L1) is kept (CRYMT_GI_DIRLIGHT 1 and 2), bit 1
    // the six directions too (2, once made).
    uint32_t VoxFlags() const { return (Gi().dirLight >= 1 ? 1u : 0u) | (Gi().dirLight == 2 && m_voxSixReady ? 2u : 0u); }
    // What the GI's cones use (visAreas[0].z): 0 one colour, 1 L1, 2 six directions.
    int ConeDirMode() const { return Gi().dirLight == 2 && m_voxSixReady ? 2 : Gi().dirLight >= 1 ? 1 : 0; }

    // The six directions (CRYMT_GI_DIRLIGHT 2): per level the light going out towards +X, -X, +Y, -Y, +Z, -Z, R11G11B10
    // with the grid's mips, made when first asked for (~220 MB for four levels). Their contents start undefined: the
    // grid is reset once (every cell "entered", which clears them).
    bool EnsureVoxSix()
    {
        if (m_voxSixReady || m_voxSixFailed)
            return m_voxSixReady;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        d.Width = d.Height = kVoxN;
        d.DepthOrArraySize = kVoxN;
        d.MipLevels = kVoxMips;
        d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = S_OK;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            for (int f = 0; f < 6 && SUCCEEDED(hr); f++)
                hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                    __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_voxSix[l][f]));
        m_voxSixReady = SUCCEEDED(hr);
        m_voxSixFailed = !m_voxSixReady;
        if (m_voxSixReady)
            for (auto& level : m_voxPrevGrid)
                for (int& v : level)
                    v = -(1 << 28); // the next update resets every cell (and its six directions)
        Log("voxel six directions (CRYMT_GI_DIRLIGHT 2): %s", m_voxSixReady ? "ready" : "failed");
        return m_voxSixReady;
    }

    // A UAV of one mip of a level's direction face f (m_voxSix), or a null 3D view.
    void SixUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int f, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 || level < 0 || !m_voxSixReady ? nullptr : m_voxSix[level][f], nullptr, &ud, at);
    }

    // A UAV of one mip of a level's per-axis opacity (grid mip mip + 1), or a null 3D view.
    void DirUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 ? nullptr : m_voxDir[level], nullptr, &ud, at);
    }

    void NullSrv(D3D12_CPU_DESCRIPTOR_HANDLE at)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(nullptr, &nd, at);
    }

    // The constants both voxel passes share: the camera (worldToView, toWorld), projection, grid.
    void VoxConstants(float* c, const float* toWorld, UINT width, UINT height, float mode, int level = 0)
    {
        memcpy(c, m_giView, 16 * sizeof(float));
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 4; i++)
                c[32 + j * 4 + i] = toWorld[i * 4 + j];
        const float rest[16] = {static_cast<float>(width), static_cast<float>(height), 1.0f / m_giProj[0], 1.0f / m_giProj[5],
                                m_giProj[10], m_giProj[14], Gi().voxelSize * static_cast<float>(1 << level), mode,
                                static_cast<float>(m_voxGrid[level][0]), static_cast<float>(m_voxGrid[level][1]), static_cast<float>(m_voxGrid[level][2]),
                                static_cast<float>(kVoxN), static_cast<float>(m_voxPrevGrid[level][0]), static_cast<float>(m_voxPrevGrid[level][1]),
                                static_cast<float>(m_voxPrevGrid[level][2]), 0.0f};
        memcpy(c + 44, rest, sizeof(rest));
    }

    // Grid levels are updated every frame (0), every 2nd (1), 4th (2), 8th (3); all of them while the grid is new. The
    // geometry pass voxelizes a level only in the frames it is updated.
    static constexpr int kVoxPeriod[kVoxLevels] = {1, 2, 4, 8};
    bool VoxUpdates(int level) const
    {
        static constexpr uint64_t kPhase[kVoxLevels] = {0, 0, 1, 3};
        return !m_voxValid || (m_giFrame % kVoxPeriod[level]) == kPhase[level];
    }

    // ---- GI geometry: the z-prepass draws voxelized into level 0 (kVoxGeomShader)
    struct VoxDraw
    {
        const std::vector<uint8_t>* vsCode;
        uint64_t vsHash;
        Layout12* layout;
        D3D10_PRIMITIVE_TOPOLOGY topology;
        D3D12_VERTEX_BUFFER_VIEW vb[16];
        UINT vbCount;
        D3D12_INDEX_BUFFER_VIEW ib;
        bool indexed;
        UINT count, instances, start, startInstance;
        INT base;
        D3D12_GPU_VIRTUAL_ADDRESS cb[kRootCbSlots[kVS]];
        bool foliage; // the z-prepass pixel shader discards (alpha test): leaves, grass
        bool dissolve; // it discards for the LOD dissolve only (no diffuse map): solid
        bool animated; // skinned, per-instance streams, vertices written by the CPU, an unreadable object matrix: every time
        uint64_t key;  // what it puts into the grid: shader, buffers, range, the object's matrix in the world
        const VS12* vs;                   // its outputs (the material pixel shader's input)
        int texture;                      // the z-prepass PS's first 2D texture slot, -1 none
        D3D12_GPU_DESCRIPTOR_HANDLE psSrv; // that PS's texture table as applied
        // The shared default-heap pages of its vertex and index buffers: the replay moves them to GENERIC_READ like a
        // normal draw (MarkPageRead). Bound without that, a page was promoted implicitly and a later copy into it in
        // the same list assumed COMMON; or the replay drew from a page left in COPY_DEST (D3D12 debug layer, 2026-11-11).
        // Dedicated pages are skipped: they are deleted with their buffer, which may happen before the replay.
        BufferPage* pages[17];
        UINT pageCount;
        // The main list it was recorded in. Its constants, dynamic vertices and texture table live in rings that free a
        // list's part once the GPU finished that list; a ring running full submits, waits and frees mid-frame. The replay
        // skips a draw whose list is done: its data may already be overwritten (GPU hangs in dense places, 2026-11-12:
        // only with the voxel geometry on, 5 of 8 runs at village's start).
        uint64_t serial;
    };
    struct VoxCameraPatch
    {
        int slot = -1;
        UINT offset = 0; // floats
        bool zero = false, transposed = false; // the camera-relative matrix (view without translation); stored transposed
    };
    struct VoxCameraCopy
    {
        std::vector<uint8_t> bytes;
        std::vector<VoxCameraPatch> patches;
        // Shadow maps: their vertex shader divides the depth row by the light frustum's far distance (cb5
        // PER_SHADOWGEN, GS_VS_FrustrumInfo.y: o0.z = (row 2 . p) / y * w), so the orthographic depth row is multiplied
        // by it. Without it every shadow draw landed in the first slice, which is left out: no geometry from the shadow
        // maps at all (a capture's vertex shader showed it).
        float zScale = 1.0f;
    };

    // A z-prepass draw of the scene: before this frame's GI pass, into the scene's depth with the R32F target.
    // (Measured on the gitest demo: ~750 such draws a frame, rtv0 R32_FLOAT with the scene's depth.)
    bool VoxRecording() const
    {
        return Gi().on && Gi().geometry && m_giInScene && m_giLastRun != m_giFrame && m_dsv && m_dsv->info == m_giSceneDepth && m_rtv[0] &&
               m_rtv[0]->format == DXGI_FORMAT_R32_FLOAT;
    }

    static bool MatrixNear(const float* data, const float* m, bool transposed)
    {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
            {
                const float a = data[r * 4 + c], b = transposed ? m[c * 4 + r] : m[r * 4 + c];
                const float d = a > b ? a - b : b - a, s = b > 0 ? b : -b;
                if (!(d <= 0.01f * (s > 1 ? s : 1))) // the engine's has the DLSS jitter, ours not
                    return false;
            }
        return true;
    }

    // Where the bound vertex shader reads this frame's view-projection: its constant buffers searched for the matrix and
    // the camera-relative one (the view without translation), as stored or transposed. None: the shader takes its
    // matrices per object (composite) or another camera (the weapon); it is not voxelized.
    std::vector<VoxCameraPatch> FindCameraMatrices() const
    {
        float vp[16], viewZero[16], vpZero[16];
        MatMul(m_giView, m_giProj, vp);
        memcpy(viewZero, m_giView, sizeof(viewZero));
        viewZero[12] = viewZero[13] = viewZero[14] = 0;
        MatMul(viewZero, m_giProj, vpZero);
        return FindMatrices(vp, vpZero);
    }

    // A shadow map's draws: the light's view-projection (the renderer's modelview, its projection is the identity),
    // and the same for positions relative to the camera ((p - eye) * vpZero = p * vp).
    std::vector<VoxCameraPatch> FindShadowMatrices() const
    {
        float toWorld[16], vpZero[16];
        RigidInverse(m_giView, toWorld);
        memcpy(vpZero, m_shadowMv, sizeof(vpZero));
        for (int j = 0; j < 4; j++)
            vpZero[12 + j] += toWorld[12] * m_shadowMv[j] + toWorld[13] * m_shadowMv[4 + j] + toWorld[14] * m_shadowMv[8 + j];
        return FindMatrices(m_shadowMv, vpZero);
    }

    // The engine cube's face draws: the face camera's view-projection (the renderer's modelview and projection, which the
    // stream sends before the draws), and the same for positions relative to the face's eye (the cube sits at the eye).
    std::vector<VoxCameraPatch> FindCubeMatrices() const
    {
        float vp[16], viewZero[16], vpZero[16];
        MatMul(m_passMv, m_passProj, vp);
        memcpy(viewZero, m_passMv, sizeof(viewZero));
        viewZero[12] = viewZero[13] = viewZero[14] = 0;
        MatMul(viewZero, m_passProj, vpZero);
        return FindMatrices(vp, vpZero);
    }

    // Between CryMT's "CryMT engine cube" marks (the engine drawing a face of its cube).
    bool CubeRecording() const
    {
        return m_engineCubePass && Gi().on && Gi().geometry && (Gi().cubeGeometry || Gi().cubeInject) && m_giHaveCamera && m_voxOcc[0];
    }

    std::vector<VoxCameraPatch> FindMatrices(const float* vp, const float* vpZero) const
    {
        std::vector<VoxCameraPatch> found;
        const StageState& st = m_stage[kVS];
        for (UINT slot = 0; slot < kRootCbSlots[kVS]; slot++)
        {
            const Buffer12* b = (m_vs->cbMask >> slot & 1) ? st.cb[slot] : nullptr;
            if (!b || !b->Dynamic() || !b->slice.cpu)
                continue;
            const auto* f = reinterpret_cast<const float*>(b->slice.cpu);
            for (UINT o = 0; o + 16 <= b->desc.ByteWidth / 4; o += 4)
                for (int k = 0; k < 4; k++)
                    if (MatrixNear(f + o, (k & 2) ? vpZero : vp, k & 1))
                    {
                        found.push_back({static_cast<int>(slot), o, (k & 2) != 0, (k & 1) != 0});
                        break;
                    }
        }
        return found;
    }

    int m_zDraws = 0, m_firstWaterZ = -1; // this frame's z-prepass draws seen by RecordVoxDraw, the first ocean one among them
    uint64_t m_voxDrawsFrame = ~0ull;     // the frame (g_presented) whose z-prepass draws m_voxDraws holds

    // kind: 0 the z-prepass, 1 a shadow map (CRYMT_GI_SHADOWGEOM), 2 the engine cube's face (CRYMT_GI_CUBEGEOM).
    void RecordVoxDraw(int kind, bool indexed, UINT count, UINT instances, UINT start, INT base, UINT startInstance)
    {
        const bool shadow = kind == 1, cube = kind == 2;
        if (cube)
        {
            // The face's own targets and camera, at its first scene draw (depth written into an HDR target): what the
            // inject pass reads at the face's end.
            if (!m_cubeFaceNoted && m_dsv && m_dsv->info && m_rtv[0] && m_rtv[0]->info && m_ps &&
                m_rtv[0]->format == DXGI_FORMAT_R16G16B16A16_FLOAT && !(m_depth && m_depth->d12.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO))
            {
                m_cubeFaceNoted = true;
                m_cubeFaceDepth = m_dsv->info;
                m_cubeFaceDepthFormat = m_dsv->format;
                m_cubeFaceColour = m_rtv[0]->info;
                MatMul(m_passMv, m_passProj, m_cubeFaceVp);
                memcpy(m_cubeFaceProj, m_passProj, sizeof(m_cubeFaceProj));
            }
            if (!Gi().cubeGeometry)
                return;
            m_stats.voxCubeSeen++;
            if (m_cubeDrawsFrame != g_presented) // a frame's face draws the voxelization did not take (GI voxels off)
            {
                m_cubeDrawsFrame = g_presented;
                m_cubeDraws.clear();
                m_cubeCopies.clear();
                m_cubeKeys.clear();
            }
        }
        if (kind == 0 && m_vs)
        {
            m_zDraws++;
            if (m_vs->water && m_firstWaterZ < 0)
                m_firstWaterZ = m_zDraws;
        }
        // Draws that leave no depth are no geometry: the sky's z-prepass draw (Sky.cfx ZPass: no pixel shader, depth
        // writes off) is a box around the camera and stood in the voxels as one (the user: a cube like a sky dome in
        // the Num2 view once voxel geometry was on by default).
        if ((m_depth && m_depth->d12.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO) || (!shadow && !m_ps) || (cube && !m_depth))
        {
            m_stats.voxGeomSkipped++;
            return;
        }
        if (m_gs || m_vs->srvEnd > 0 || !m_layout || m_layout->slotEnd > 16)
        {
            m_stats.voxGeomSkipped++;
            return;
        }
        // The first-person view (weapon, hands: their own depth range under kWeaponDepth) is not the world's geometry.
        if (kind == 0 && m_numViewports > 0 && m_viewports[0].MaxDepth < 0.5f)
        {
            m_stats.voxGeomWeapon++;
            return;
        }
        // The ocean's surface (its waves) is no geometry for the GI: the user saw voxels on the waves.
        if (m_vs->water)
        {
            m_stats.voxGeomSkipped++;
            return;
        }
        auto& cameras = shadow ? m_shadowCamera : cube ? m_cubeCamera : m_voxCamera;
        // Alpha-tested materials (the z-prepass pixel shader discards): leaves, grass. With their texture (the material
        // pass) they are voxelized by the shape of the leaves; CRYMT_GI_GEOMETRY=2 keeps them also without one.
        // Alpha-tested: the z-prepass discards by the diffuse map's alpha (CommonZPass.cfi %_RT_ALPHATEST). Its LOD
        // dissolve (%_RT_DISSOLVE, on most objects: clip by a noise texture) discards too, and was taken for foliage:
        // the ground and most solid objects went in half open and cut by their diffuse map's alpha (gloss there), the
        // user's holes across roads and dark spots (the capture: every level-0 stamp had foliage's opacity).
        const bool foliage = m_ps && m_ps->discards && m_ps->diffuseSlot >= 0;
        // The cube's foliage stays out: the cells it missed off screen were solid (mine, 2026-11-13: stamps 255).
        if (cube && foliage)
        {
            m_stats.voxCubeFoliage++;
            return;
        }
        if (kind == 0)
            m_stats.voxClass[(m_ps && m_ps->discards ? 2 : 0) + (m_ps && m_ps->diffuseSlot >= 0 ? 1 : 0)]++;
        auto it = cameras.find(m_vs->contentHash);
        if (it == cameras.end())
        {
            std::vector<VoxCameraPatch> found = shadow ? FindShadowMatrices() : cube ? FindCubeMatrices() : FindCameraMatrices();
            if (cube && found.empty()) // not kept: the face's matrices may not have come yet for this shader's first draw
            {
                m_stats.voxCubeNoCamera++;
                return;
            }
            it = cameras.emplace(m_vs->contentHash, std::move(found)).first;
        }
        if (kind == 0 && !foliage)
            m_stats.voxPatch[it->second.empty() ? 0 : 1 + (it->second[0].zero ? 2 : 0) + (it->second[0].transposed ? 1 : 0)]++;
        if (it->second.empty())
        {
            (shadow ? m_stats.voxShadowNoCamera : cube ? m_stats.voxCubeNoCamera : m_stats.voxGeomNoCamera)++;
            return;
        }
        VoxDraw d{};
        d.serial = g_mainSerial;
        d.vsCode = &m_vs->code;
        d.vsHash = m_vs->contentHash;
        d.layout = m_layout;
        d.topology = m_topology;
        d.vbCount = m_layout->slotEnd;
        memcpy(d.vb, m_vbApplied, d.vbCount * sizeof(D3D12_VERTEX_BUFFER_VIEW));
        d.ib = m_ibApplied;
        d.indexed = indexed;
        auto notePage = [&d](Buffer12* b) {
            BufferPage* p = b ? b->Page() : nullptr;
            if (!p || p->dedicated)
                return;
            for (UINT k = 0; k < d.pageCount; k++)
                if (d.pages[k] == p)
                    return;
            if (d.pageCount < std::size(d.pages))
                d.pages[d.pageCount++] = p;
        };
        for (UINT i = 0; i < d.vbCount; i++)
            notePage(m_vb[i].buffer);
        if (indexed)
            notePage(m_ib);
        d.count = count;
        d.instances = instances;
        d.start = start;
        d.base = base;
        d.startInstance = startInstance;
        d.foliage = foliage;
        d.dissolve = m_ps && m_ps->discards && !foliage;
        d.vs = m_vs;
        // The material's colour from its diffuse map (the dissolve variant reads only its noise texture: none then).
        d.texture = m_ps && m_ps->diffuseSlot >= 0 && m_stage[kPS].srvTable.ptr ? m_ps->diffuseSlot : -1;
        d.psSrv = m_stage[kPS].srvTable;
        const StageState& st = m_stage[kVS];
        memcpy(d.cb, st.cbApplied, sizeof(d.cb));
        // Its identity, for voxelizing unchanged objects only now and then: the shader, the buffers and the range, and the
        // object's matrix (PI_ObjMatrix, 3x4 at c0 of VS slot 3) with the translation in the world (the camera-relative
        // matrices moved back by the eye), 2 cm / 0.001 steps. A spinning radar, a door, a falling piece: a new matrix every
        // frame, a new key. Skinned characters, foliage the player bends, cloth: bones, per-instance or CPU-written vertex
        // streams, so animated (every time).
        d.animated = m_layout->animated;
        if (d.animated)
            m_stats.voxAnimatedWhy[0]++;
        for (UINT i = 0; i < d.vbCount && !d.animated; i++)
            if (m_vb[i].buffer && m_vb[i].buffer->Dynamic())
            {
                d.animated = true;
                m_stats.voxAnimatedWhy[1]++;
            }
        uint64_t key = HashWords(&d.vsHash, sizeof(d.vsHash), 1469598103934665603ull);
        key = HashWords(d.vb, d.vbCount * sizeof(D3D12_VERTEX_BUFFER_VIEW), key);
        if (indexed)
            key = HashWords(&d.ib, sizeof(d.ib), key);
        const UINT range[6] = {count, instances, start, static_cast<UINT>(base), startInstance, 0};
        key = HashWords(range, sizeof(range), key);
        if (m_vs->cbMask >> 3 & 1)
        {
            const Buffer12* object = st.cb[3];
            const bool dyn = object && object->Dynamic() && object->slice.cpu && object->desc.ByteWidth >= 48;
            if (dyn || (object && object->headKnown))
            {
                float m[12];
                memcpy(m, dyn ? static_cast<const void*>(object->slice.cpu) : static_cast<const void*>(object->head), sizeof(m));
                if (it->second[0].zero)
                {
                    float eye[16];
                    RigidInverse(m_giView, eye);
                    m[3] += eye[12];
                    m[7] += eye[13];
                    m[11] += eye[14];
                }
                int32_t q[12];
                for (int i = 0; i < 12; i++)
                {
                    const float x = m[i] * ((i & 3) == 3 ? 50.0f : 1000.0f);
                    q[i] = x > -2e9f && x < 2e9f ? static_cast<int32_t>(x + (x >= 0 ? 0.5f : -0.5f)) : 0x7FFFFFFF;
                }
                key = HashWords(q, sizeof(q), key);
            }
            else if (!d.animated)
            {
                d.animated = true;
                m_stats.voxAnimatedWhy[object ? 3 : 2]++;
            }
        }
        d.key = key;
        // A face draws an object in several passes (lights, layers): once is enough.
        if (cube && !m_cubeKeys.insert(key).second)
        {
            m_stats.voxCubeRepeat++;
            return;
        }
        // The camera buffers' contents now (the replay patches the matrices in): once per buffer and frame.
        for (const VoxCameraPatch& p : it->second)
        {
            const Buffer12* b = st.cb[p.slot];
            if (!b || !b->slice.cpu || b->Gpu() != d.cb[p.slot])
            {
                m_stats.voxGeomSkipped++;
                return;
            }
            VoxCameraCopy& copy = (shadow ? m_shadowCopies : cube ? m_cubeCopies : m_voxCameraCopies)[d.cb[p.slot]];
            if (copy.bytes.empty())
            {
                copy.bytes.assign(b->slice.cpu, b->slice.cpu + b->desc.ByteWidth);
                const Buffer12* frustum = st.cb[5];
                if (shadow && (m_vs->cbMask >> 5 & 1) && frustum && frustum->Dynamic() && frustum->slice.cpu)
                {
                    const float lightFar = reinterpret_cast<const float*>(frustum->slice.cpu)[1];
                    if (lightFar > 1.0f && lightFar < 1e9f)
                        copy.zScale = lightFar;
                }
            }
            bool known = false;
            for (const VoxCameraPatch& q : copy.patches)
                known = known || (q.offset == p.offset && q.zero == p.zero);
            if (!known)
                copy.patches.push_back(p);
        }
        if (cube && m_cubeDraws.empty())
            RigidInverse(m_passMv, m_cubeToWorld); // the face's eye and axes (its grid and camera-relative positions)
        (shadow ? m_shadowDraws : cube ? m_cubeDraws : m_voxDraws).push_back(d);
        if (shadow)
            m_stats.voxShadowRecorded++;
        else if (cube)
            m_stats.voxCubeRecorded++;
    }

    // ---- GI geometry from the shadow maps (CRYMT_GI_SHADOWGEOM): the light passes draw the casters around the player,
    // behind the camera too. The renderer's matrices come in stream order before each draw that follows a change
    // (kOpMatrices); a shadow map (vf207, Prep_PrepareDepthMap) puts the light's view-projection on the modelview stack
    // and the identity as the projection. Each frame one job: a grid level (0..2) along one axis, all nine in nine
    // frames, from the smallest of last frame's maps that covers the level around the eye (else the widest). Cells keep
    // their opacity between jobs (the update lowers it only where the screen sees through), so static geometry stays in
    // the grid while moving objects come from the z-prepass every frame.
    // The sun's shadows in the fog: at a shadow map's first draw after its matrix came (PassMatrices) the map, its light
    // matrix and far distance (the shadow vertex shaders' cb5 [1], GS_VS_FrustrumInfo.y: they write the depth as the
    // light-space z over it) go into m_sunMaps by map. Only single-sample D32 maps (the cascades; the +-2 km map is D24S8);
    // a map drawn for another light leaves the table.
    void NoteSunMap()
    {
        m_vfogShadowPending = false;
        if (!m_dsv || !m_dsv->info || m_dsv->info->buffer)
            return;
        const auto* t = static_cast<const TextureCore*>(m_dsv->info);
        int slot = -1, oldest = 0;
        for (int i = 0; i < kSunMaps; i++)
        {
            if (m_sunMaps[i].map == m_dsv->info)
                slot = i;
            if (m_sunMaps[i].frame < m_sunMaps[oldest].frame)
                oldest = i;
        }
        float lightFar = 0;
        const Buffer12* f = m_vs && (m_vs->cbMask >> 5 & 1) ? m_stage[kVS].cb[5] : nullptr;
        if (f && f->Dynamic() && f->slice.cpu && f->desc.ByteWidth >= 8)
            lightFar = reinterpret_cast<const float*>(f->slice.cpu)[1];
        const bool sun = m_vfogPendingSun && t->samples == 1 && (t->format == DXGI_FORMAT_R32_TYPELESS || t->format == DXGI_FORMAT_D32_FLOAT) &&
                         lightFar > 1.0f && lightFar < 1e9f;
        if (!sun)
        {
            if (slot >= 0)
                m_sunMaps[slot] = SunMap{};
            return;
        }
        SunMap& s = m_sunMaps[slot >= 0 ? slot : oldest];
        s.map = m_dsv->info;
        memcpy(s.mv, m_vfogPendingMv, sizeof(s.mv));
        s.lightFar = lightFar;
        s.frame = m_frame;
    }

    bool ShadowRecording()
    {
        if (m_vfogShadowPending)
            NoteSunMap();
        if (m_shadowCurrent >= 0 && m_shadowCurrent < 8)
            m_stats.voxShadowMapDraws[m_shadowCurrent]++;
        return m_shadowRecording && m_dsv && m_dsv->info != m_giSceneDepth;
    }

    // Half the width a light matrix covers around the eye, less the eye's offset from its centre (m; 0: not around it).
    float ShadowCover(const float* m, const float* eye) const
    {
        float c[4];
        for (int j = 0; j < 4; j++)
            c[j] = eye[0] * m[j] + eye[1] * m[4 + j] + eye[2] * m[8 + j] + m[12 + j];
        if (!(c[3] > 0))
            return 0;
        const float ex = std::fabs(c[0] / c[3]), ey = std::fabs(c[1] / c[3]);
        const float sx = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]), sy = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
        if (ex >= 1 || ey >= 1 || !(sx > 0) || !(sy > 0))
            return 0;
        return std::min(c[3] / sx * (1 - ex), c[3] / sy * (1 - ey));
    }

    void PassMatrices(const float* m)
    {
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(m_passMv, m, sizeof(m_passMv));
        memcpy(m_passProj, m + 16, sizeof(m_passProj));
        m_shadowRecording = false;
        m_shadowCurrent = -1;
        const bool shadow = !memcmp(m_passProj, kIdentity, sizeof(kIdentity)) && memcmp(m_passMv, kIdentity, sizeof(kIdentity));
        // The sun's shadows in the fog: a shadow map's matrix for its next draw (NoteSunMap); the sun's has w ~ 1e6 (the
        // light ~1000 km out along the sun), a lamp's a few metres.
        m_vfogShadowPending = shadow;
        m_vfogPendingSun = shadow && std::fabs(m_passMv[15]) > 1e4f;
        if (shadow)
            memcpy(m_vfogPendingMv, m_passMv, sizeof(m_vfogPendingMv));
        if (!shadow || !Gi().on || !Gi().geometry || !Gi().shadowGeometry || !m_giHaveCamera)
            return;
        if (m_shadowFrame != m_giFrame) // the frame's first map (m_giFrame advances in the GI pass)
        {
            m_shadowFrame = m_giFrame;
            m_shadowDraws.clear();
            m_shadowCopies.clear();
            m_shadowLastMaps = std::min(m_shadowMaps, 8);
            memcpy(m_shadowLastCover, m_shadowCover, sizeof(m_shadowCover));
            m_stats.voxShadowCoverCount = static_cast<uint32_t>(m_shadowLastMaps);
            memcpy(m_stats.voxShadowCover, m_shadowCover, sizeof(m_shadowCover));
            m_shadowMaps = 0;
            const int job = static_cast<int>(m_giFrame % 9);
            m_shadowLevel = job % 3;
            m_shadowAxis = job / 3;
            const float need = 0.87f * static_cast<float>(kVoxN) * Gi().voxelSize * static_cast<float>(1 << m_shadowLevel); // the level's cube seen from any side
            m_shadowPick = -1;
            for (int i = 0; i < m_shadowLastMaps; i++)
            {
                const float c = m_shadowLastCover[i], b = m_shadowPick >= 0 ? m_shadowLastCover[m_shadowPick] : -1.0f;
                if (c > 0 && (m_shadowPick < 0 || (c >= need ? (b < need || c < b) : (b < need && c > b))))
                    m_shadowPick = i;
            }
            m_stats.voxShadowJobs++;
            if (m_shadowPick >= 0 && m_shadowLastCover[m_shadowPick] >= need)
                m_stats.voxShadowCovered++;
        }
        else if (m_shadowMaps > 0 && !memcmp(m_passMv, m_shadowMv, sizeof(m_shadowMv)))
        {
            m_shadowRecording = m_shadowMaps - 1 == m_shadowPick; // back to the same map
            m_shadowCurrent = m_shadowMaps - 1;
            return;
        }
        memcpy(m_shadowMv, m_passMv, sizeof(m_shadowMv));
        float toWorld[16];
        RigidInverse(m_giView, toWorld);
        if (m_giFrame == 1000 && GetEnvironmentVariableW(L"CRYMT_SHADOWDUMP", nullptr, 0)) // diagnostics: the frame's maps
        {
            const float* x = m_passMv;
            Log("shadow map %d: eye %.2f %.2f %.2f, dsv %ux%u fmt %d, rtv %d\n  %g %g %g %g\n  %g %g %g %g\n  %g %g %g %g\n  %g %g %g %g\n", m_shadowMaps,
                toWorld[12], toWorld[13], toWorld[14], m_dsv ? static_cast<const TextureCore*>(m_dsv->info)->width : 0u,
                m_dsv ? static_cast<const TextureCore*>(m_dsv->info)->height : 0u, m_dsv ? static_cast<int>(static_cast<const TextureCore*>(m_dsv->info)->format) : -1,
                m_rtv[0] ? static_cast<int>(m_rtv[0]->format) : -1, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9], x[10], x[11], x[12], x[13], x[14], x[15]);
        }
        if (m_shadowMaps < 8)
        {
            m_shadowCover[m_shadowMaps] = ShadowCover(m_passMv, toWorld + 12);
            m_stats.voxShadowMapSeen[m_shadowMaps]++;
            m_stats.voxShadowMapCover[m_shadowMaps] += m_shadowCover[m_shadowMaps];
        }
        m_shadowRecording = m_shadowMaps == m_shadowPick;
        m_shadowCurrent = m_shadowMaps;
        m_shadowMaps++;
        m_stats.voxShadowMaps++;
    }

    bool EnsureVoxGeom()
    {
        if (m_voxGeomFailed)
            return false;
        if (m_voxOcc[0])
            return true;
        m_voxGeomFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        ID3DBlob* errors = nullptr;
        if (!compile || !g_serializeRoot ||
            FAILED(compile(kVoxGeomShader, strlen(kVoxGeomShader), "CryMT_VoxGeom", nullptr, nullptr, "voxelize", "ps_5_0", 1 << 15, 0, &m_voxGeomPs, &errors)))
        {
            Log("voxel geometry shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            return false;
        }
        // The main root signature's parameters (the engine's vertex shaders bind through them) plus the pass's own.
        static const D3D12_SHADER_VISIBILITY kVisibility[kStages] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_GEOMETRY,
                                                                     D3D12_SHADER_VISIBILITY_PIXEL};
        D3D12_ROOT_PARAMETER params[kRootParams + 2]{};
        D3D12_DESCRIPTOR_RANGE ranges[kStages][2]{};
        for (int s = 0; s < kStages; s++)
        {
            for (UINT i = 0; i < kRootCbSlots[s]; i++)
            {
                D3D12_ROOT_PARAMETER& p = params[kRootCb[s] + i];
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                p.Descriptor.ShaderRegister = i;
                p.ShaderVisibility = kVisibility[s];
            }
            ranges[s][0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTableSrvs, 0, 0, 0};
            // The pixel stage runs our shaders here, which sample through the static s15 alone.
            ranges[s][1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, s == kPS ? kTableSamplers - 1 : kTableSamplers, 0, 0, 0};
            params[kRootSrv[s]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kRootSrv[s]].DescriptorTable = {1, &ranges[s][0]};
            params[kRootSrv[s]].ShaderVisibility = kVisibility[s];
            params[kRootSampler[s]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kRootSampler[s]].DescriptorTable = {1, &ranges[s][1]};
            params[kRootSampler[s]].ShaderVisibility = kVisibility[s];
        }
        const D3D12_DESCRIPTOR_RANGE uav = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0}; // occupancy, albedo
        params[kVoxGeomUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kVoxGeomUav].DescriptorTable = {1, &uav};
        params[kVoxGeomUav].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[kVoxGeomConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kVoxGeomConstants].Constants = {13, 0, 8};
        params[kVoxGeomConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC material{};
        material.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        material.AddressU = material.AddressV = material.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.MaxLOD = D3D12_FLOAT32_MAX;
        material.ShaderRegister = 15;
        material.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = kRootParams + 2;
        rd.pParameters = params;
        rd.NumStaticSamplers = 1;
        rd.pStaticSamplers = &material;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
        ID3DBlob* blob = nullptr;
        if (FAILED(g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&m_voxGeomRoot))))
        {
            Log("voxel geometry root signature failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        blob->Release();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        d.Width = d.Height = kVoxN;
        d.DepthOrArraySize = kVoxN;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_UINT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = S_OK;
        for (int l = 0; l < kVoxGeomLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxOcc[l]));
        d.Format = DXGI_FORMAT_R8_UINT;
        for (int l = 0; l < kVoxGeomLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxSeen[l]));
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int l = 0; l < kVoxGeomLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxAlbedo[l]));
        m_voxCompile = reinterpret_cast<void*>(compile);
        Log("voxel geometry grids %d levels of %d^3: %s", kVoxGeomLevels, kVoxN, SUCCEEDED(hr) ? "ready" : "failed");
        m_voxGeomFailed = FAILED(hr);
        return SUCCEEDED(hr);
    }

    // The albedo grid of a level, or a null 3D view (level -1).
    void AlbedoUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int level)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(level >= 0 ? m_voxAlbedo[level] : nullptr, nullptr, &ud, at);
    }

    // The material pixel shader for a vertex shader's outputs and a texture slot: its input declares the outputs alike
    // (register order, component counts), samples the texture at the first TEXCOORD with two or more components through
    // a static linear wrap sampler (s15), drops texels under alpha 0.5 for alpha-tested materials (job.w), stamps the
    // cell as kVoxGeomShader and stores the texture's colour as the cell's albedo (u1). Null when the outputs have no
    // such coordinates or the compile fails (the draw then takes kVoxGeomShader).
    ID3DBlob* MaterialPs(const VS12* vs, int texture, int& compiled)
    {
        const uint64_t key = vs->contentHash * 131 + static_cast<uint64_t>(texture);
        auto it = m_voxMaterialPs.find(key);
        if (it != m_voxMaterialPs.end())
            return it->second;
        if (compiled >= 4 || !m_voxCompile)
            return nullptr;
        compiled++;
        std::vector<const VS12::Output*> outs;
        for (const VS12::Output& o : vs->outputs)
            outs.push_back(&o);
        std::sort(outs.begin(), outs.end(), [](const VS12::Output* a, const VS12::Output* b) {
            return a->reg != b->reg ? a->reg < b->reg : (a->mask & -a->mask) < (b->mask & -b->mask);
        });
        std::string fields, position, uv;
        for (size_t i = 0; i < outs.size(); i++)
        {
            const VS12::Output& o = *outs[i];
            const int count = __builtin_popcount(o.mask & 15);
            const char* type = o.type == D3D10_REGISTER_COMPONENT_UINT32 ? "uint" : o.type == D3D10_REGISTER_COMPONENT_SINT32 ? "int" : "float";
            char line[160];
            snprintf(line, sizeof(line), "    %s%s%d v%zu : %s%u;\n", o.type == D3D10_REGISTER_COMPONENT_FLOAT32 ? "" : "nointerpolation ", type,
                     count < 1 ? 1 : count, i, o.name.c_str(), o.index);
            fields += line;
            const bool isTexcoord = _strnicmp(o.name.c_str(), "TEXCOORD", 8) == 0;
            if (o.value == D3D10_NAME_POSITION)
                position = "i.v" + std::to_string(i);
            else if (uv.empty() && isTexcoord && count >= 2 && o.type == D3D10_REGISTER_COMPONENT_FLOAT32 && (o.mask & 3) == 3)
                uv = "i.v" + std::to_string(i) + ".xy";
        }
        ID3DBlob* blob = nullptr;
        if (!position.empty() && !uv.empty())
        {
            char head[256];
            snprintf(head, sizeof(head), "Texture2D<float4> material : register(t%d);\n", texture);
            const std::string src = std::string(kVoxMaterialHead) + head + "struct In\n{\n" + fields + "};\n" + kVoxMaterialBody1 + position +
                                    kVoxMaterialBody2 + uv + kVoxMaterialBody3;
            using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
            ID3DBlob* errors = nullptr;
            if (FAILED(reinterpret_cast<CompileFn>(m_voxCompile)(src.data(), src.size(), "CryMT_VoxMaterial", nullptr, nullptr, "voxelize", "ps_5_0", 1 << 15, 0, &blob, &errors)))
            {
                LogLimited("voxmaterial", "voxel material shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
                blob = nullptr;
            }
            if (errors)
                errors->Release();
        }
        m_voxMaterialPs.emplace(key, blob);
        if (blob)
            m_stats.voxMaterialShaders++;
        return blob;
    }

    // The seen grid of a level, or a null 3D view (level -1).
    void SeenUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int level)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8_UINT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(level >= 0 ? m_voxSeen[level] : nullptr, nullptr, &ud, at);
    }

    // The occupancy grid of a level, or a null 3D view (level -1).
    void OccupancyUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int level)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(level >= 0 ? m_voxOcc[level] : nullptr, nullptr, &ud, at);
    }

    // At most a few new pipelines a frame (created on this thread); draws without one wait for a later frame.
    ID3D12PipelineState* VoxGeomPso(const VoxDraw& d, ID3DBlob* ps, int& created)
    {
        const int type = d.topology == D3D10_PRIMITIVE_TOPOLOGY_POINTLIST                                                   ? 1
                         : d.topology == D3D10_PRIMITIVE_TOPOLOGY_LINELIST || d.topology == D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP ? 2
                         : d.topology == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST || d.topology == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ? 3
                                                                                                                                 : 0;
        if (!type)
            return nullptr;
        const uint64_t key = HashBytes(&d.layout->contentHash, sizeof(uint64_t), d.vsHash * 31 + static_cast<uint64_t>(type)) ^
                             reinterpret_cast<uintptr_t>(ps) * 0x9E3779B97F4A7C15ull;
        auto it = m_voxGeomPsos.find(key);
        if (it != m_voxGeomPsos.end())
            return it->second;
        if (created >= 8)
            return nullptr;
        created++;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = m_voxGeomRoot;
        p.VS = {d.vsCode->data(), d.vsCode->size()};
        p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.InputLayout = {d.layout->elements.data(), static_cast<UINT>(d.layout->elements.size())};
        p.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(type);
        p.SampleDesc = {1, 0};
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(g.dev->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso))))
            pso = nullptr; // e.g. a vertex shader whose position is not its first output: kept as null, not tried again
        m_voxGeomPsos.emplace(key, pso);
        m_stats.voxGeomPsos++;
        return pso;
    }

    // Inside RunGi, before the grid update: the recorded z-prepass draws into the occupancy grids of levels 0-2 (their cells,
    // toroidal), projected along x, y or z (one axis a frame, the update takes stamps of the last three frames).
    // The engine cube's face just drawn into the geometry levels (kVoxShader "inject", CRYMT_GI_CUBEINJECT): at its "end"
    // mark, while its depth and HDR colour targets still hold it. Cells outside the camera's view where the face shows a
    // surface get the cube's stamp and the face's colour.
    void RunCubeInject()
    {
        ResourceInfo* depth = m_cubeFaceDepth;
        ResourceInfo* colour = m_cubeFaceColour;
        m_cubeFaceNoted = false;
        if (!Gi().on || !Gi().cubeInject || !m_voxValid || !m_voxPsoInject || !m_voxOcc[0] || !depth || !colour || !depth->res ||
            !colour->res || m_cubeFaceDepthFormat == DXGI_FORMAT_UNKNOWN)
            return;
        const D3D12_RESOURCE_DESC dd = depth->res->GetDesc(), cd = colour->res->GetDesc();
        if (dd.Width != cd.Width || dd.Height != cd.Height || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            m_stats.voxInjectSkipped++;
            return;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        VoxTable(cpu, gpu, kVoxGeomLevels);
        Require(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FlushBarriers();
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            const int b = l * static_cast<int>(kVoxTable);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DepthSrvFormat(m_cubeFaceDepthFormat);
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            g.dev->CreateShaderResourceView(depth->res, &sd, at(b));
            sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g.dev->CreateShaderResourceView(colour->res, &sd, at(b + 1));
            NullSrv(at(b + 2));
            VoxUav(at(b + 3), 0, l);
            OccupancyUav(at(b + 4), l);
            VoxUav(at(b + 5), -1);
            SeenUav(at(b + 6), l);
            AlbedoUav(at(b + 7), l);
            VoxUav(at(b + 8), -1);
            L1Uav(at(b + 9), 0, l);
            L1Uav(at(b + 10), -1);
            for (int f = 0; f < 6; f++)
            {
                SixUav(at(b + 11 + f), f, 0, l);
                SixUav(at(b + 17 + f), f, -1);
            }
        }
        // The face's depth to distance: clip.w = s z (s = +-1 the projection's handedness), d = A s + B / distance.
        const float A = m_cubeFaceProj[10], B = m_cubeFaceProj[14], s = m_cubeFaceProj[11] >= 0 ? 1.0f : -1.0f;
        m_list->SetComputeRootSignature(m_voxRoot);
        m_list->SetComputeRoot32BitConstant(3, VoxFlags(), 0);
        m_list->SetPipelineState(m_voxPsoInject);
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            float c[kVoxConstants] = {};
            VoxConstants(c, m_cubeFaceVp, m_giFullW, m_giFullH, 0.0f, l); // worldToView, invProj: the camera's
            memcpy(c + 16, m_cubeFaceVp, 16 * sizeof(float));                // worldToPrevClip: the face's view-projection
            c[32] = -A * s;                                                   // toWorldX: its depth (x, y), size
            c[33] = B;
            c[34] = static_cast<float>(dd.Width);
            c[35] = static_cast<float>(dd.Height);
            for (int i = 0; i < 3; i++)
                c[52 + i] = c[56 + i] = static_cast<float>(m_voxPrevGrid[l][i]); // the level's cells as they stand
            m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + static_cast<UINT64>(l) * kVoxTable * g.srvInc});
            m_list->Dispatch(kVoxN / 4, kVoxN / 4, kVoxN / 4);
            D3D12_RESOURCE_BARRIER uav[3] = {};
            for (D3D12_RESOURCE_BARRIER& u : uav)
                u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav[0].UAV.pResource = m_vox[l];
            uav[1].UAV.pResource = m_voxOcc[l];
            uav[2].UAV.pResource = m_voxSeen[l];
            m_list->ResourceBarrier(3, uav);
        }
        m_stats.voxInjectRuns++;
        m_psoApplied = nullptr;
    }

    // cubeOnly: the engine cube's face just drawn (its "end" mark: its draws' data is still current), toWorld its camera.
    void RunVoxelGeometry(const float* toWorld, uint32_t stamp, bool cubeOnly = false)
    {
        if (cubeOnly ? (!Gi().geometry || m_cubeDraws.empty() || !EnsureVoxGeom())
                     : (!Gi().geometry || (m_voxDraws.empty() && m_shadowDraws.empty()) || !EnsureVoxGeom()))
        {
            if (!cubeOnly)
            {
                m_voxDraws.clear();
                m_voxCameraCopies.clear();
                m_shadowDraws.clear();
                m_shadowCopies.clear();
            }
            m_cubeDraws.clear();
            m_cubeCopies.clear();
            m_cubeKeys.clear();
            return;
        }
        // Jobs: the z-prepass draws into each level updated this frame along the frame's axis; the shadow job's draws
        // into its level along its axis.
        struct Job
        {
            const std::vector<VoxDraw>* draws;
            const std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, VoxCameraCopy>* copies;
            int level, axis;
            bool shadow;
            int first[3];
            std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS> patched;
            bool cube = false;
        };
        std::vector<Job> jobs;
        if (!cubeOnly && !m_voxDraws.empty())
            for (int l = 0; l < kVoxGeomLevels; l++)
                if (VoxUpdates(l))
                    jobs.push_back({&m_voxDraws, &m_voxCameraCopies, l, static_cast<int>(m_giFrame % 3), false, {}, {}});
        if (!cubeOnly && !m_shadowDraws.empty())
            jobs.push_back({&m_shadowDraws, &m_shadowCopies, m_shadowLevel, m_shadowAxis, true, {}, {}});
        // The cube's face into every geometry level, along an axis that turns between the face's visits (a face comes
        // every 5 or 6 runs: the turn count plus its sixth).
        if (cubeOnly)
        {
            const int axis = static_cast<int>((m_cubeTurn + m_cubeTurn / 6) % 3);
            m_cubeTurn++;
            for (int l = 0; l < kVoxGeomLevels; l++)
            {
                Job job{&m_cubeDraws, &m_cubeCopies, l, axis, false, {}, {}};
                job.cube = true;
                jobs.push_back(std::move(job));
                m_stats.voxCubeJobs++;
            }
        }
        // Row-vector orthographic projections: grid axis U -> clip x, V -> clip y (down the viewport), W -> depth 0..1.
        // The camera buffers with the matrices replaced, per job, uploaded before any state is set (an upload may start
        // a new list).
        static const int kU[3] = {1, 0, 0}, kV[3] = {2, 2, 1}, kW[3] = {0, 1, 2};
        for (Job& job : jobs)
        {
            const int l = job.level, u = kU[job.axis], v = kV[job.axis], w = kW[job.axis];
            const float cell = Gi().voxelSize * static_cast<float>(1 << l), span = cell * kVoxN;
            float corner[3];
            for (int i = 0; i < 3; i++)
            {
                const float q = toWorld[12 + i] / cell;
                job.first[i] = static_cast<int>(q) - (static_cast<float>(static_cast<int>(q)) > q ? 1 : 0) - kVoxN / 2; // as RunVoxelUpdate
                corner[i] = static_cast<float>(job.first[i]) * cell;
            }
            float ortho[16] = {}, orthoZero[16];
            ortho[u * 4 + 0] = 2 / span;
            ortho[12 + 0] = -2 * corner[u] / span - 1;
            ortho[v * 4 + 1] = -2 / span;
            ortho[12 + 1] = 2 * corner[v] / span + 1;
            ortho[w * 4 + 2] = 1 / span;
            ortho[12 + 2] = -corner[w] / span;
            ortho[15] = 1;
            // For positions relative to the camera: (p - eye) * orthoZero = p * ortho.
            memcpy(orthoZero, ortho, sizeof(ortho));
            for (int j = 0; j < 4; j++)
                orthoZero[12 + j] += toWorld[12] * ortho[j] + toWorld[13] * ortho[4 + j] + toWorld[14] * ortho[8 + j];
            for (const auto& [address, copy] : *job.copies)
            {
                const UploadAlloc up = FrameUpload((copy.bytes.size() + 255) & ~size_t{255}, 256);
                if (!up.cpu)
                    continue;
                memcpy(up.cpu, copy.bytes.data(), copy.bytes.size());
                auto* f = reinterpret_cast<float*>(up.cpu);
                for (const VoxCameraPatch& p : copy.patches)
                {
                    const float* m = p.zero ? orthoZero : ortho;
                    float scaled[16];
                    if (copy.zScale != 1.0f)
                    {
                        memcpy(scaled, m, sizeof(scaled));
                        for (int i = 0; i < 4; i++)
                            scaled[i * 4 + 2] *= copy.zScale; // the depth output's column (row vectors)
                        m = scaled;
                    }
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++)
                            f[p.offset + r * 4 + c] = p.transposed ? m[c * 4 + r] : m[r * 4 + c];
                }
                job.patched[address] = up.res->GetGPUVirtualAddress() + up.offset;
            }
        }
        // (Unchanged objects every 4th frame, CRYMT_GI_GEOMCACHE, flickered - a cell shared with foliage or a moving
        // object took the newest stamp - and was removed 2026-11-15: tag experiments-2026-11-15.)
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        VoxTable(cpu, gpu);
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            OccupancyUav({cpu.ptr + 2 * l * g.srvInc}, l);
            AlbedoUav({cpu.ptr + (2 * l + 1) * g.srvInc}, l);
        }
        FlushBarriers();
        m_list->SetGraphicsRootSignature(m_voxGeomRoot);
        const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(kVoxN), static_cast<float>(kVoxN), 0, 1};
        const D3D12_RECT scissor = {0, 0, kVoxN, kVoxN};
        m_list->RSSetViewports(1, &viewport);
        m_list->RSSetScissorRects(1, &scissor);
        m_list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        int created = 0, compiled = 0;
        ID3D12PipelineState* applied = nullptr;
        for (const Job& job : jobs)
        {
            const int l = job.level;
            const bool counted = !job.shadow && !job.cube && l == 0; // the z-prepass stats once per draw
            // The cube's stamps: frame 1 (never newer than a z-prepass stamp: InterlockedMax keeps the z-prepass's) and
            // opacity byte 253 (kVoxShader update: counted only outside the camera's view).
            const float constants[8] = {static_cast<float>(job.first[0]), static_cast<float>(job.first[1]), static_cast<float>(job.first[2]),
                                        static_cast<float>(kVoxN), static_cast<float>(job.axis), job.cube ? 1.0f : static_cast<float>(stamp), 0, 0};
            m_list->SetGraphicsRoot32BitConstants(kVoxGeomConstants, 8, constants, 0);
            m_list->SetGraphicsRootDescriptorTable(kVoxGeomUav, {gpu.ptr + 2 * l * g.srvInc});
            // Solid geometry first, then foliage, each under its own name (captures tell them apart).
            for (int part = 0; part < 2; part++)
            {
            if (job.cube && part == 1)
                continue; // no foliage from the cube
            PassName(job.cube ? "CryMT GI geometry: cube solid"
                     : job.shadow ? (part ? "CryMT GI geometry: shadow foliage" : "CryMT GI geometry: shadow solid")
                                  : (part ? "CryMT GI geometry: foliage" : "CryMT GI geometry: solid"));
            for (size_t di = 0; di < job.draws->size(); di++)
            {
                const VoxDraw& d = (*job.draws)[di];
                if (d.foliage != (part == 1))
                    continue;
                if (d.serial != g_mainSerial && SerialDone(d.serial)) // its list is done: its ring data may be reused
                {
                    m_stats.voxGeomStale++;
                    continue;
                }
                // The material shader where the z-prepass PS reads a texture; else the plain one, which alpha-tested
                // materials only get with CRYMT_GI_GEOMETRY=2 (whole cards would fill the cells).
                ID3DBlob* ps = d.texture >= 0 && Gi().materials ? MaterialPs(d.vs, d.texture, compiled) : nullptr;
                if (!ps && d.foliage && !Gi().geometryFoliage)
                {
                    if (counted)
                        m_stats.voxGeomFoliage++;
                    continue;
                }
                ID3D12PipelineState* pso = VoxGeomPso(d, ps ? ps : m_voxGeomPs, created);
                if (!pso && ps)
                    pso = VoxGeomPso(d, ps = m_voxGeomPs, created); // the outputs did not link: plain
                if (!pso)
                {
                    m_stats.voxGeomNoPso++;
                    continue;
                }
                if (pso != applied)
                    m_list->SetPipelineState(applied = pso);
                if (ps != m_voxGeomPs)
                {
                    m_list->SetGraphicsRootDescriptorTable(kRootSrv[kPS], d.psSrv);
                    if (counted)
                        m_stats.voxGeomMaterial++;
                }
                // Opacity: foliage CRYMT_GI_FOLIAGE, solid 1 (254 / 255 with the LOD dissolve: a mark in captures).
                const float job2[2] = {job.cube ? 253.5f / 255.0f : d.foliage ? Gi().foliageOpacity : d.dissolve ? 254.0f / 255.0f : 1.0f,
                                       d.foliage ? 1.0f : 0.0f};
                m_list->SetGraphicsRoot32BitConstants(kVoxGeomConstants, 2, job2, 6);
                for (UINT k = 0; k < d.pageCount; k++)
                    MarkPageRead(d.pages[k]);
                FlushBarriers();
                m_list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(d.topology));
                m_list->IASetVertexBuffers(0, d.vbCount, d.vb);
                if (d.indexed)
                    m_list->IASetIndexBuffer(&d.ib);
                for (UINT i = 0; i < kRootCbSlots[kVS]; i++)
                    if (d.cb[i])
                    {
                        const auto p = job.patched.find(d.cb[i]);
                        m_list->SetGraphicsRootConstantBufferView(kRootCb[kVS] + i, p != job.patched.end() ? p->second : d.cb[i]);
                    }
                if (d.indexed)
                    m_list->DrawIndexedInstanced(d.count, d.instances, d.start, d.base, d.startInstance);
                else
                    m_list->DrawInstanced(d.count, d.instances, d.start, d.startInstance);
                if (counted)
                    m_stats.voxGeomDraws++;
                else if (job.shadow)
                    m_stats.voxShadowDraws++;
                else if (job.cube)
                    m_stats.voxCubeDraws++;
                if (counted && !d.foliage)
                    m_stats.voxGeomSolid++;
            }
            PassNameEnd();
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = m_voxOcc[l];
            m_list->ResourceBarrier(1, &barrier);
        }
        if (!cubeOnly)
        {
            m_voxDraws.clear();
            m_voxCameraCopies.clear();
            m_shadowDraws.clear();
            m_shadowCopies.clear();
        }
        m_cubeDraws.clear();
        m_cubeCopies.clear();
        m_cubeKeys.clear();
        m_psoApplied = nullptr; // RunGi ends with RestoreListState (the cube's face: its "end" mark calls it)
    }

    // Inside RunGi: this frame's depth and the previous frame's colour into the grid.
    void RunVoxelUpdate(TextureCore* depth, const D3D12_SHADER_RESOURCE_VIEW_DESC& depthSrv, const float* toWorld, const float* reproj)
    {
        if (!EnsureVox())
        {
            GiStamp(4);
            return;
        }
        if (Gi().dirLight == 2)
            EnsureVoxSix();
        // Geometry stamps (frames + 1, kept within float precision for the constants); the update takes the last three.
        const uint32_t stamp = static_cast<uint32_t>(m_giFrame & 0xFFFFFF) + 1;
        RunVoxelGeometry(toWorld, stamp);
        GiStamp(4);
        const bool geometry = Gi().geometry && m_voxOcc[0];
        // The lamps for the light pass (b1), uploaded before any state is set (an upload may start a new list).
        D3D12_GPU_VIRTUAL_ADDRESS lamps = 0;
        const uint32_t lampCount = Gi().lampLight > 0 ? m_giLampCount : 0;
        if (geometry)
        {
            const UploadAlloc up = FrameUpload(512, 256);
            if (up.cpu)
            {
                auto* f = reinterpret_cast<float*>(up.cpu);
                memset(f, 0, 512);
                for (uint32_t k = 0; k < lampCount; k++)
                {
                    const float* s = m_giLamps[k];
                    memcpy(f + k * 4, s, 4 * sizeof(float)); // position, radius
                    for (int i = 0; i < 3; i++)
                        f[32 + k * 4 + i] = s[4 + i] * Gi().lampLight; // colour
                    f[32 + k * 4 + 3] = s[7];                          // cosine of the half angle
                    memcpy(f + 64 + k * 4, s + 8, 3 * sizeof(float));  // direction
                }
                f[96] = static_cast<float>(lampCount);
                lamps = up.res->GetGPUVirtualAddress() + up.offset;
            }
        }
        // Level 0 every frame, level 1 every 2nd, level 2 every 4th, level 3 every 8th (the first frames: all of them).
        bool update[kVoxLevels] = {VoxUpdates(0), VoxUpdates(1), VoxUpdates(2), VoxUpdates(3)};
        UINT tables = 0;
        for (int l = 0; l < kVoxLevels; l++)
            tables += update[l] ? kVoxMips : 0; // the update, then one table per mip (u0 the level below, u2 the level)
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        VoxTable(cpu, gpu, tables);
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        D3D12_UNORDERED_ACCESS_VIEW_DESC nu{};
        nu.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        FlushBarriers();
        m_list->SetComputeRootSignature(m_voxRoot);
        m_list->SetComputeRoot32BitConstant(3, VoxFlags(), 0);
        UINT table = 0;
        for (int l = 0; l < kVoxLevels; l++)
        {
            if (!update[l])
                continue;
            const float vs = Gi().voxelSize * static_cast<float>(1 << l);
            for (int i = 0; i < 3; i++)
            {
                const float q = toWorld[12 + i] / vs;
                const int cell = static_cast<int>(q) - (static_cast<float>(static_cast<int>(q)) > q ? 1 : 0); // floor
                m_voxGrid[l][i] = cell - kVoxN / 2;
            }
            const UINT first = table;
            for (UINT t = 0; t < kVoxMips; t++, table++)
            {
                const int b = static_cast<int>(table * kVoxTable);
                if (t == 0)
                {
                    g.dev->CreateShaderResourceView(depth->res, &depthSrv, at(b));
                    g.dev->CreateShaderResourceView(m_giColor, nullptr, at(b + 1));
                }
                else
                {
                    NullSrv(at(b));
                    NullSrv(at(b + 1));
                }
                // t2: the foliage mask for the update (leaves are not walls; nothing filled behind them), when it fits.
                if (t == 0 && m_foliageSrv && m_foliageW == m_giFullW && m_foliageH == m_giFullH)
                    g.dev->CreateShaderResourceView(m_foliageSrv->info->res, nullptr, at(b + 2));
                else
                    NullSrv(at(b + 2));
                VoxUav(at(b + 3), t == 0 ? 0 : static_cast<int>(t) - 1, l);
                if (t == 0)
                    OccupancyUav(at(b + 4), geometry && l < kVoxGeomLevels ? l : -1); // the update's u1 (a null 3D view without geometry)
                else
                    g.dev->CreateUnorderedAccessView(nullptr, nullptr, &nu, at(b + 4));
                VoxUav(at(b + 5), t == 0 ? -1 : static_cast<int>(t), l);
                if (t == 0)
                    SeenUav(at(b + 6), geometry && l < kVoxGeomLevels ? l : -1); // the update's and light's u3
                else
                    DirUav(at(b + 6), t >= 2 ? static_cast<int>(t) - 2 : -1, l); // the downsample's dirSrc
                if (t == 0)
                    AlbedoUav(at(b + 7), geometry && l < kVoxGeomLevels ? l : -1); // the light's u4
                else
                    DirUav(at(b + 7), static_cast<int>(t) - 1, l); // the downsample's dirDst
                if (t == 0 && l == 0)
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC rd{};
                    rd.Format = DXGI_FORMAT_R8_UNORM;
                    rd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    rd.Texture3D.WSize = static_cast<UINT>(-1);
                    g.dev->CreateUnorderedAccessView(m_voxRoof, nullptr, &rd, at(b + 8)); // the roof pass's u5
                }
                else
                    VoxUav(at(b + 8), -1);
                L1Uav(at(b + 9), t == 0 ? 0 : static_cast<int>(t) - 1, l); // u6: the level's light direction (downsample: the mip below)
                L1Uav(at(b + 10), t == 0 ? -1 : static_cast<int>(t), l);   // u7: the downsample's mip
                for (int f = 0; f < 6; f++)
                {
                    SixUav(at(b + 11 + f), f, t == 0 ? 0 : static_cast<int>(t) - 1, l); // u8..u13: the six directions (the mip below)
                    SixUav(at(b + 17 + f), f, t == 0 ? -1 : static_cast<int>(t), l);   // u14..u19: the downsample's mip
                }
            }
            float c[kVoxConstants] = {};
            VoxConstants(c, toWorld, m_giFullW, m_giFullH, 0.0f, l);
            MatMul(m_giView, reproj, c + 16); // world -> this view -> the previous clip space
            if (geometry && l < kVoxGeomLevels) // prevGridMin.w: the oldest stamp taken, the level's last three voxelizations
            {
                const uint32_t window = static_cast<uint32_t>(3 * kVoxPeriod[l] - 1);
                c[59] = static_cast<float>(stamp > window ? stamp - window : 1);
                c[51] = (static_cast<float>(stamp) + (Gi().opacityGeometry && Gi().geometry ? 0.5f : 0.0f)) * (Gi().geometryPersist ? -1.0f : 1.0f); // mode: this
                // frame's stamp, a half: the opacity from the geometry only
                                                                                             // (entered cells keep only it);
                                                                                             // negative: geometry kept
            }
            m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            m_list->SetPipelineState(m_voxPsoUpdate);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + first * kVoxTable * g.srvInc});
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = m_vox[l];
            m_list->ResourceBarrier(1, &uav);
            m_list->Dispatch(kVoxN / 4, kVoxN / 4, kVoxN / 4);
            m_list->ResourceBarrier(1, &uav);
            // Every update of the geometry's levels, also without sun and lamps (a pass that ran only while a lamp was
            // in the frame's list rewrote these cells and left them as they were by turns: flicker in mine).
            if (geometry && lamps && l < kVoxGeomLevels && Gi().sunAlbedo > 0)
            {
                // Sunlight for the geometry's cells: sun direction, colour and albedo in place of worldToPrevClip;
                // the lamps in b1.
                float lc[kVoxConstants];
                memcpy(lc, c, sizeof(lc));
                const float lit = m_giSun[3] > 0 ? 1.0f : 0.0f;
                const float sun[12] = {m_giSun[0], m_giSun[1], m_giSun[2], 0, m_giSun[4] * lit, m_giSun[5] * lit, m_giSun[6] * lit, 0,
                                       Gi().sunAlbedo, Gi().skyFill, Gi().lampTest ? 1.0f : 0.0f, static_cast<float>(Gi().sunMarch)};
                memcpy(lc + 16, sun, sizeof(sun));
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, lc, 0);
                m_list->SetComputeRootConstantBufferView(2, lamps);
                m_list->SetPipelineState(m_voxPsoLight);
                m_list->Dispatch(kVoxN / 4, kVoxN / 4, kVoxN / 4);
                m_list->ResourceBarrier(1, &uav);
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            }
            if (geometry && l == 0 && m_voxPsoRoof && Gi().roofCells)
            {
                // Cover above (kVoxShader "roof"): an eighth of the texture's z slabs a frame, mode the slab.
                float rc[kVoxConstants];
                memcpy(rc, c, sizeof(rc));
                rc[51] = static_cast<float>(m_giFrame % 8);
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, rc, 0);
                m_list->SetPipelineState(m_voxPsoRoof);
                m_list->Dispatch(kVoxN / 8, kVoxN / 8, kVoxN / 64);
                D3D12_RESOURCE_BARRIER rb{};
                rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                rb.UAV.pResource = m_voxRoof;
                m_list->ResourceBarrier(1, &rb);
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            }
            m_list->SetPipelineState(m_voxPsoDown);
            float dc[kVoxConstants];
            memcpy(dc, c, sizeof(dc));
            dc[3] = static_cast<float>(Gi().mipOpacity); // worldToView[0].w: the downsample's opacity rule
            for (UINT mip = 1; mip < kVoxMips; mip++)
            {
                dc[51] = static_cast<float>(mip); // mode: the mip written
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, dc, 0);
                m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + (first + mip) * kVoxTable * g.srvInc});
                const UINT groups = ((kVoxN >> mip) + 3) / 4;
                m_list->Dispatch(groups, groups, groups);
                D3D12_RESOURCE_BARRIER both[2] = {uav, uav};
                both[1].UAV.pResource = m_voxDir[l];
                m_list->ResourceBarrier(2, both);
            }
            memcpy(m_voxPrevGrid[l], m_voxGrid[l], sizeof(m_voxGrid[l]));
        }
        m_voxValid = true;
        m_stats.voxPasses++;
        if (g_voxSnapWanted.load(std::memory_order_relaxed) && m_voxSnap.empty())
            RecordVoxSnap(toWorld);
    }

    static DXGI_FORMAT VoxViewFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return f;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    // The panel (CRYMT_GI_KEYS=1, the launcher's in-game keys): tabs of live settings drawn with GDI (Consolas, bold,
    // about 1/60 of the screen height) into a texture when they change, at the left, centred in height. Num/ shows or
    // hides it, Num7 / Num9 the tab, Num8 / Num2 the row, Num4 / Num6 its value -/+ (held: repeats), Num5 its default;
    // Num+ / Num- move the time of day on any tab (CryMT.cpp). A value off its default (the ini's, CryMT's) is orange
    // with a star. One PanelItem per setting: its value an int in lo..hi (toggles 0..1, lists index `names`, scales
    // count steps of `scale`); get / def / set read and write g_giControl (set runs on main, on the keys;
    // GiApplyControl applies the values at present).
    struct PanelItem
    {
        int tab;
        const char* label; // "русский|English"
        int lo, hi, step;
        bool wrap;                // lists and toggles go round, scales stop at their ends
        const char* const* names; // per value "русский|English", or null: the value times `scale` by `format`
        float scale;
        const char* format;
        int (*get)();
        int (*def)();
        void (*set)(int);
        bool mark;                         // highlighted off its default (not the time of day)
        std::string (*text)(bool ru);      // its own text, or null
    };
    static constexpr int kPanelTabs = 6;
    static std::string PanelLang(const char* s, bool ru)
    {
        const char* bar = strchr(s, '|');
        return !bar ? std::string(s) : ru ? std::string(s, bar - s) : std::string(bar + 1);
    }
    static bool PanelRussian()
    {
        static const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
        return ru;
    }
    static const char* PanelTabName(int t)
    {
        static const char* const names[kPanelTabs] = {"GI|GI", "AO|AO", "Воксели|Voxels", "Отражения|Reflections", "Движок|Engine", "Время|Time"};
        return names[t];
    }
    static bool EnvDefaultOn() { return EnvModeBase() == 1; }
    static int BaseParticleOut()
    {
        static const int values[5] = {50, 70, 100, 150, 200};
        const int pct = static_cast<int>(GiBase().particleOutdoor * 100.0f + 0.5f);
        int best = 0;
        for (int i = 1; i < 5; i++)
            if (abs(values[i] - pct) < abs(values[best] - pct))
                best = i;
        return best;
    }
    // The screen-space shadows' reach on the panel (metres): the list's entry nearest a value.
    static constexpr int kSssReaches[6] = {100, 200, 500, 1000, 2000, 4000};
    static constexpr float kSssWeaponThick[5] = {0.005f, 0.02f, 0.05f, 0.1f, 0.2f}; // the panel's list for the weapon and hands
    static constexpr int kGiNears[4] = {30, 40, 50, 62}; // the panel's "GI at full strength up to" (metres; 62: the field's top)
    // The AO page's "Накопление GTAO" index for the ini / environment value: 0 off, 1 0.5, 2 0.7, 3 0.9 (the nearest).
    static int GtaoHistoryIndex(const GiSettings& s)
    {
        if (!s.gtaoHistory)
            return 0;
        return s.gtaoHistoryWeight < 0.6f ? 1 : s.gtaoHistoryWeight < 0.8f ? 2 : 3;
    }
    static int GiNearIndex(float metres)
    {
        int best = 0;
        for (int i = 1; i < 4; i++)
            if (fabsf(static_cast<float>(kGiNears[i]) - metres) < fabsf(static_cast<float>(kGiNears[best]) - metres))
                best = i;
        return best;
    }
    static int SssWeaponIndex(float t)
    {
        int best = 0;
        for (int i = 1; i < 5; i++)
            if (fabsf(kSssWeaponThick[i] - t) < fabsf(kSssWeaponThick[best] - t))
                best = i;
        return best;
    }
    static int SssReachIndex(float metres)
    {
        int best = 0;
        for (int i = 1; i < 6; i++)
            if (fabsf(kSssReaches[i] - metres) < fabsf(kSssReaches[best] - metres))
                best = i;
        return best;
    }
    template <int Bit> static int BakedGet()
    {
        const int v = g_giControl.bakedOff;
        return ((v < 0 ? GiBase().bakedOff : v) & Bit) ? 1 : 0;
    }
    template <int Bit> static int BakedDef() { return (GiBase().bakedOff & Bit) ? 1 : 0; }
    template <int Bit> static void BakedSet(int on)
    {
        const int v = g_giControl.bakedOff, cur = v < 0 ? GiBase().bakedOff : v;
        g_giControl.bakedOff = on ? cur | Bit : cur & ~Bit;
    }
    static const std::vector<PanelItem>& PanelItems()
    {
        static const char* const onOff[] = {"ВЫКЛ|OFF", "ВКЛ|ON"};
        static const char* const ao[] = {"SSAO ИГРЫ|GAME'S SSAO", "GTAO"};
        static const char* const gtaoAcc[] = {"ВЫКЛ|OFF", "0.5", "0.7", "0.9"};
        static const char* const contact[] = {"ВЫКЛ|OFF", "СТАРЫЕ, 12 ШАГОВ|OLD, 12 STEPS", "SSS (BEND)"};
        static const char* const sssReach[] = {"100 М|100 M", "200 М|200 M", "500 М|500 M", "1000 М|1000 M", "2000 М|2000 M", "4000 М|4000 M"};
        static const char* const sssWeapon[] = {"КАК МИР|AS THE WORLD", "+2%", "+5%", "+10%", "+20%"}; // a second window, the darker
        static const char* const outdoor[] = {"50%", "70%", "100%", "150%", "200%"};
        static const char* const baked[] = {"ДВИЖОК|ENGINE", "GI"};
        static const char* const view[] = {"ВЫКЛ|OFF", "КАРКАС|WIRE", "СПЛОШНОЙ|SOLID"};
        static const char* const geometry[] = {"ВЫКЛ|OFF", "ВКЛ|ON", "ВКЛ + ЛИСТВА|ON + FOLIAGE"};
        static const char* const opacity[] = {"ЭКРАН И ГЕОМЕТРИЯ|SCREEN+GEOMETRY", "ГЕОМЕТРИЯ|GEOMETRY"};
        static const char* const cellLight[] = {"ОДИН ЦВЕТ|ONE COLOUR", "ЦВЕТ + НАПРАВЛЕНИЕ|COLOUR + DIRECTION",
                                                "6 НАПРАВЛЕНИЙ|6 DIRECTIONS"};
        static const char* const env[] = {"КАРТЫ ИГРЫ|GAME'S MAPS", "ВОКСЕЛЬНЫЙ КУБ|VOXEL CUBE", "КУБ ДВИЖКА|ENGINE CUBE"};
        static const char* const water[] = {"ПЛАНАРНОЕ|PLANAR", "КУБКАРТА|CUBE MAP", "SSR + КУБКАРТА|SSR + CUBE MAP"};
        static const char* const waterSource[] = {"ВОКСЕЛИ|VOXELS", "ДВИЖОК|ENGINE"};
        static const char* const cubeCull[] = {"КАК В ДВИЖКЕ|ENGINE'S", "НЕТ|NONE", "ОБРАТНОЕ|INVERTED", "ПО ГРАНЯМ|PER FACE"};
        static const char* const cubeMip[] = {"АВТО|AUTO", "0", "1", "2", "3", "4", "5", "6", "7", "8"};
        static const char* const cubeBlend[] = {"ВЫКЛ|OFF", "50%", "25%"};
        static const char* const cubeSize[] = {"64", "128", "256", "512"};
        static const char* const giNear[] = {"30 М (ДО 75)|30 M (TO 75)", "40 М (ДО 100)|40 M (TO 100)", "50 М (ДО 125)|50 M (TO 125)",
                                             "62 М (ДО 155)|62 M (TO 155)"};
        static const char* const giMethod[] = {"КОНУСЫ|CONES", "ПРОБЫ (GI v2)|PROBES (GI v2)", "СВЕТ ПРОБ, ТЕНЬ КОНУСОВ|PROBE LIGHT, CONE SHADE"};
        static const char* const cubePicture[] = {"Num4/6: СНЯТЬ|Num4/6: TAKE", "Num4/6: СНЯТЬ|Num4/6: TAKE"};
        static const char* const cubeRate[] = {"КАЖДЫЙ КАДР|EVERY FRAME", "РАЗ В 2 КАДРА|EVERY 2ND FRAME", "РАЗ В 3 КАДРА|EVERY 3RD FRAME",
                                               "РАЗ В 4 КАДРА|EVERY 4TH FRAME", "РАЗ В 6 КАДРОВ|EVERY 6TH FRAME", "РАЗ В 8 КАДРОВ|EVERY 8TH FRAME"};
        static const int outdoorValues[5] = {50, 70, 100, 150, 200};
        static const char* const fogGi[] = {"50%", "100%", "150%", "200%"};
        static const int fogGiValues[4] = {50, 100, 150, 200};
        static const char* const fogSun[] = {"ВЫКЛ|OFF", "50%", "100%"};
        static const char* const fogHaze[] = {"ВЫКЛ|OFF", "10%", "20%", "35%", "50%"};
        static const int fogHazeValues[5] = {0, 10, 20, 35, 50};
        static const char* const fogSamples[] = {"1", "4", "8"};
        static const char* const dlssModel[] = {"K (ТРАНСФОРМЕР)|K (TRANSFORMER)", "J", "L (DLSS 4.5)|L (DLSS 4.5)", "M (DLSS 4.5)|M (DLSS 4.5)",
                                                "F (CNN)|F (CNN)", "E (CNN)|E (CNN)"};
        static const char* const dlssCompare[] = {"ВЫКЛ|OFF", "СПРАВА K|RIGHT: K", "СПРАВА J|RIGHT: J", "СПРАВА L|RIGHT: L", "СПРАВА M|RIGHT: M",
                                                  "СПРАВА F|RIGHT: F", "СПРАВА E|RIGHT: E"};
        static const std::vector<PanelItem> items = {
            // GI
            {0, "GI|GI", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.enabled ? 1 : 0; }, [] { return 1; },
             [](int v) { g_giControl.enabled = v != 0; }, true, nullptr},
            {0, "Окклюзия под укрытием|Occlusion under cover", 0, 30, 1, false, nullptr, 0.1f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.occlusionScale * 10.0f)); }, [] { return 10; },
             [](int v) { g_giControl.occlusionScale = v / 10.0f; }, true, nullptr},
            {0, "Отражённый свет|Bounce light", 0, 40, 1, false, nullptr, 0.1f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.bounceScale * 10.0f)); }, [] { return 10; },
             [](int v) { g_giControl.bounceScale = v / 10.0f; }, true, nullptr},
            {0, "На оружии и руках|On weapon and hands", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.weapon ? 1 : 0; },
             [] { return 1; }, [](int v) { g_giControl.weapon = v != 0; }, true, nullptr},
            {1, "Затенение|Ambient occlusion", 0, 1, 1, true, ao, 0, nullptr,
             [] { const int v = g_giControl.gtao; return v < 0 ? (GiBase().replaceSsao ? 1 : 0) : v; },
             [] { return GiBase().replaceSsao ? 1 : 0; }, [](int v) { g_giControl.gtao = v; }, true, nullptr},
            // GTAO's own history (the user, 2026-11-14: noisy on small objects and leaves while moving; 0.9 swam): off as
            // before (DLAA accumulates the turning pattern), or mixed with the last frame at 0.5 / 0.7 / 0.9.
            {1, "Накопление GTAO|GTAO accumulation", 0, 3, 1, false, gtaoAcc, 0, nullptr,
             [] { const int v = g_giControl.gtaoHistory; return v >= 0 ? std::min(v, 3) : GtaoHistoryIndex(GiBase()); },
             [] { return GtaoHistoryIndex(GiBase()); }, [](int v) { g_giControl.gtaoHistory = v; }, true, nullptr},
            {1, "Контактные тени|Contact shadows", 0, 2, 1, true, contact, 0, nullptr,
             [] { const int v = g_giControl.contact; return v < 0 ? BaseContact() : v; },
             [] { return BaseContact(); }, [](int v) { g_giControl.contact = v; }, true, nullptr},
            {1, "Дальность SSS|SSS reach", 0, 5, 1, false, sssReach, 0, nullptr,
             [] { const int v = g_giControl.sssReach; return SssReachIndex(v > 0 ? static_cast<float>(v) : GiBase().sssReach); },
             [] { return SssReachIndex(GiBase().sssReach); }, [](int v) { g_giControl.sssReach = kSssReaches[v]; }, true, nullptr},
            {1, "SSS: толщина рук и оружия|SSS: hands and weapon thickness", 0, 4, 1, false, sssWeapon, 0, nullptr,
             [] { const int v = g_giControl.sssWeapon; return v >= 0 ? v : SssWeaponIndex(GiBase().sssWeaponThickness); },
             [] { return SssWeaponIndex(GiBase().sssWeaponThickness); }, [](int v) { g_giControl.sssWeapon = v; }, true, nullptr},
            // How far the GI works in full; it fades out by 2.5 x that and the engine's baked occlusion comes back (the
            // user, Sponza 2026-11-13: "everything far away much lighter" - where the baked occlusion is weak).
            {0, "GI в полную силу до|GI at full strength up to", 0, 3, 1, false, giNear, 0, nullptr,
             [] { const int v = g_giControl.giNear; return v >= 0 ? v : GiNearIndex(GiBase().terrainAoNear); },
             [] { return GiNearIndex(GiBase().terrainAoNear); }, [](int v) { g_giControl.giNear = v; }, true, nullptr},
            // GI v2 (CRYMT_GI_PROBES): probes traced through the voxels near the camera in place of the cones.
            {0, "Метод GI|GI method", 0, 2, 1, true, giMethod, 0, nullptr,
             [] { const int v = g_giControl.probes; return v < 0 ? GiBase().probes : std::min(v, 2); },
             [] { return GiBase().probes; }, [](int v) { g_giControl.probes = v; }, true, nullptr},
            {0, "GI на дыме и пыли|GI on smoke and dust", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.particles; return v < 0 ? (GiBase().particlesGi ? 1 : 0) : v; },
             [] { return GiBase().particlesGi ? 1 : 0; }, [](int v) { g_giControl.particles = v; }, true, nullptr},
            {0, "Дым снаружи темнее|Smoke darker outdoors", 0, 4, 1, true, outdoor, 0, nullptr,
             [] {
                 const int v = g_giControl.particleOut;
                 if (v < 0)
                     return BaseParticleOut();
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(outdoorValues[i] - v) < abs(outdoorValues[best] - v))
                         best = i;
                 return best;
             },
             [] { return BaseParticleOut(); }, [](int v) { g_giControl.particleOut = outdoorValues[v]; }, true, nullptr},
            // Volumetric fog, stage 1 (RunVfog): the engine's fog darkened where the GI sees shade (rooms, under cover).
            {0, "GI в тумане|GI in the fog", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.vfog; return v < 0 ? std::min(GiBase().vfog, 1) : std::min(v, 1); },
             [] { return std::min(GiBase().vfog, 1); }, [](int v) { g_giControl.vfog = v; }, true, nullptr},
            {0, "Сила GI в тумане|GI strength in the fog", 0, 3, 1, false, fogGi, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogGi >= 0 ? g_giControl.vfogGi.load() : static_cast<int>(GiBase().vfogGi * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 4; i++)
                     if (abs(fogGiValues[i] - v) < abs(fogGiValues[best] - v))
                         best = i;
                 return best;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogGi * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 4; i++)
                     if (abs(fogGiValues[i] - v) < abs(fogGiValues[best] - v))
                         best = i;
                 return best;
             },
             [](int v) { g_giControl.vfogGi = fogGiValues[v]; }, true, nullptr},
            // Stage 2: the sun's shadow maps in the fog (its haze dark in the shade outdoors too).
            {0, "Тени солнца в тумане|Sun shadows in the fog", 0, 2, 1, true, fogSun, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogSun >= 0 ? g_giControl.vfogSun.load() : static_cast<int>(GiBase().vfogSun * 100.0f + 0.5f);
                 return v <= 25 ? 0 : v <= 75 ? 1 : 2;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogSun * 100.0f + 0.5f);
                 return v <= 25 ? 0 : v <= 75 ? 1 : 2;
             },
             [](int v) { g_giControl.vfogSun = v * 50; }, true, nullptr},
            // The sun's shadow samples per cell (smoother shadow edges in the fog, a little more time).
            {0, "Выборки теней в тумане|Fog shadow samples", 0, 2, 1, true, fogSamples, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogSamples >= 0 ? g_giControl.vfogSamples.load() : GiBase().vfogSamples;
                 return v >= 8 ? 2 : v >= 4 ? 1 : 0;
             },
             [] { return GiBase().vfogSamples >= 8 ? 2 : GiBase().vfogSamples >= 4 ? 1 : 0; },
             [](int v) { g_giControl.vfogSamples = v == 2 ? 8 : v == 1 ? 4 : 1; }, true, nullptr},
            // The near air: a haze of its own near the camera (the sun's shafts in a thin fog).
            {0, "Плотность воздуха вблизи|Near air density", 0, 4, 1, false, fogHaze, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogHaze >= 0 ? g_giControl.vfogHaze.load() : static_cast<int>(GiBase().vfogHaze * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(fogHazeValues[i] - v) < abs(fogHazeValues[best] - v))
                         best = i;
                 return best;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogHaze * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(fogHazeValues[i] - v) < abs(fogHazeValues[best] - v))
                         best = i;
                 return best;
             },
             [](int v) { g_giControl.vfogHaze = fogHazeValues[v]; }, true, nullptr},
            // Its own darkening outdoors (the smoke's row above is the particles' only).
            {0, "Туман снаружи темнее|Fog darker outdoors", 0, 4, 1, true, outdoor, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogOut >= 0 ? g_giControl.vfogOut.load() : static_cast<int>(GiBase().vfogOutdoor * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(outdoorValues[i] - v) < abs(outdoorValues[best] - v))
                         best = i;
                 return best;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogOutdoor * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(outdoorValues[i] - v) < abs(outdoorValues[best] - v))
                         best = i;
                 return best;
             },
             [](int v) { g_giControl.vfogOut = outdoorValues[v]; }, true, nullptr},
            {1, "Запечённый AO: объекты|Baked AO: objects (RAM)", 0, 1, 1, true, baked, 0, nullptr, BakedGet<2>, BakedDef<2>,
             BakedSet<2>, true, nullptr},
            {1, "Запечённый AO: небо|Baked AO: objects' sky", 0, 1, 1, true, baked, 0, nullptr, BakedGet<4>, BakedDef<4>,
             BakedSet<4>, true, nullptr},
            {1, "Запечённый AO: деревья|Baked AO: under trees", 0, 1, 1, true, baked, 0, nullptr, BakedGet<8>, BakedDef<8>,
             BakedSet<8>, true, nullptr},
            {1, "Запечённый AO: террейн|Baked AO: terrain", 0, 1, 1, true, baked, 0, nullptr, BakedGet<16>, BakedDef<16>,
             BakedSet<16>, true, nullptr},
            // Voxels
            {2, "Вид вокселей|Voxel view", 0, 2, 1, true, view, 0, nullptr, [] { return g_giControl.view.load(); }, [] { return 0; },
             [](int v) { g_giControl.view = v; }, true, nullptr},
            {2, "Геометрия в вокселях|Geometry in voxels", 0, 2, 1, true, geometry, 0, nullptr,
             [] {
                 const int v = g_giControl.geometry;
                 return v >= 0 ? v : GiBase().geometry ? (GiBase().geometryFoliage ? 2 : 1) : 0;
             },
             [] { return GiBase().geometry ? (GiBase().geometryFoliage ? 2 : 1) : 0; }, [](int v) { g_giControl.geometry = v; }, true,
             nullptr},
            {2, "Непрозрачность из|Opacity from", 0, 1, 1, true, opacity, 0, nullptr,
             [] { const int v = g_giControl.voxOpacity; return v < 0 ? (GiBase().opacityGeometry ? 1 : 0) : v; },
             [] { return GiBase().opacityGeometry ? 1 : 0; }, [](int v) { g_giControl.voxOpacity = v; }, true, nullptr},
            // The cells' light (CRYMT_GI_DIRLIGHT): one colour for all directions, or a colour and the direction it goes
            // out stronger (a wall lit outside stays dark inside).
            {2, "Свет ячейки|Cell light", 0, 2, 1, true, cellLight, 0, nullptr,
             [] { const int v = g_giControl.dirLight; return v < 0 ? GiBase().dirLight : std::min(v, 2); },
             [] { return GiBase().dirLight; }, [](int v) { g_giControl.dirLight = v; }, true, nullptr},
            // Reflections
            {3, "Отражения|Reflections", 0, 2, 1, true, env, 0, nullptr,
             [] { const int v = g_giControl.env; return v < 0 ? EnvModeBase() : v; }, [] { return EnvModeBase(); },
             [](int v) { g_giControl.env = v; }, true, nullptr},
            {3, "Отражение воды|Water reflection", 0, 2, 1, true, water, 0, nullptr, [] { return WaterMode(); },
             [] { return WaterModeBase(); },
             [](int v) {
                 g_giControl.waterCube = v;
                 CryMTConsoleCommand(v ? "r_WaterReflections 0" : "r_WaterReflections 1"); // the engine's planar pass
             },
             true, nullptr},
            {3, "Кубкарта воды|Water cube map", 0, 1, 1, true, waterSource, 0, nullptr, [] { return EngineCubeOn() ? 1 : 0; },
             [] { return 1; }, [](int v) { g_giControl.waterSource = v; }, true, nullptr},
            {3, "Грань кубкарты рисуется|Cube map face drawn", 0, 5, 1, false, cubeRate, 0, nullptr,
             [] { const int v = g_giControl.envCubeRate; return EnvCubeRateIndex(v > 0 ? v : EnvCubeRateBase()); },
             [] { return EnvCubeRateIndex(EnvCubeRateBase()); }, [](int v) { g_giControl.envCubeRate = kEnvCubeRates[v]; }, true, nullptr},
            {3, "Отсечение в кубкарте|Cube map culling", 0, 3, 1, true, cubeCull, 0, nullptr, [] { return EngineCubeCull(); },
             [] { return EngineCubeCullBase(); }, [](int v) { g_giControl.envCubeCull = v; }, true, nullptr},
            {3, "Mip кубкарты (отладка)|Cube map mip (debug)", 0, 9, 1, false, cubeMip, 0, nullptr,
             [] { return std::max(g_giControl.envCubeMip.load(), -1) + 1; }, [] { return 0; }, [](int v) { g_giControl.envCubeMip = v - 1; }, true,
             nullptr},
            // r_EnvCMResolution, the renderer's int at 0x382E9848 (0 64, 1 128, 2 256, 3 512): the engine makes its cube anew.
            {3, "Размер кубкарты|Cube map size", 0, 3, 1, false, cubeSize, 0, nullptr,
             [] { return std::min(std::max(*reinterpret_cast<const int*>(0x382E9848), 0), 3); }, [] { return 2; },
             [](int v) {
                 char command[48];
                 snprintf(command, sizeof(command), "r_EnvCMResolution %d", v);
                 CryMTConsoleCommand(command);
             },
             true, nullptr},
            {3, "Смешивание граней|Face blending", 0, 2, 1, true, cubeBlend, 0, nullptr, [] { return EngineCubeBlendIndex(EngineCubeBlend()); },
             [] { return EngineCubeBlendIndex(EngineCubeBlendBase()); }, [](int v) { g_giControl.envCubeBlend = v; }, true, nullptr},
            {3, "Сущности в кубкарте|Entities in the cube", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.envCubeEntities; return v < 0 ? EngineCubeEntitiesBase() : v; }, [] { return EngineCubeEntitiesBase(); },
             [](int v) { g_giControl.envCubeEntities = v; }, true, nullptr},
            // The clouds apart from the other entities (the user, 2026-11-14): off by default (cut at the faces' seams,
            // drawn inside rooms).
            {3, "Облака в кубкарте|Clouds in the cube", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.envCubeClouds; return v < 0 ? EngineCubeCloudsBase() : v; }, [] { return EngineCubeCloudsBase(); },
             [](int v) { g_giControl.envCubeClouds = v; }, true, nullptr},
            {3, "Нижняя грань кубкарты|Cube map down face", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.envCubeDown; return v < 0 ? EngineCubeDownBase() : v; }, [] { return EngineCubeDownBase(); },
             [](int v) { g_giControl.envCubeDown = v; }, true, nullptr},
            {3, "Снимок кубкарты (logs\\play)|Cube map picture (logs\\play)", 0, 1, 1, true, cubePicture, 0, nullptr, [] { return 0; },
             [] { return 0; }, [](int) { g_cubeDumpWanted = true; }, false, nullptr},
            {3, "Небо в отражении воды|Sky in the water's reflection", 2, 20, 1, false, nullptr, 0.1f, "x%.1f",
             [] { return static_cast<int>(lroundf(WaterSky() * 10.0f)); }, [] { return static_cast<int>(lroundf(WaterSkyBase() * 10.0f)); },
             [](int v) { g_giControl.waterSky = v / 10.0f; }, true, nullptr},
            {3, "На всех материалах|On every material", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.envAll ? 1 : 0; },
             [] { return 1; }, [](int v) { g_giControl.envAll = v != 0; }, true, nullptr},
            {3, "Материалы со своей картой|Materials with a map", 0, 16, 1, false, nullptr, 0.5f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.envOwnScale * 2.0f)); }, [] { return 2; },
             [](int v) { g_giControl.envOwnScale = v / 2.0f; }, true, nullptr},
            {3, "Все материалы|Every material", 0, 16, 1, false, nullptr, 0.5f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.envAllScale * 2.0f)); }, [] { return 2; },
             [](int v) { g_giControl.envAllScale = v / 2.0f; }, true, nullptr},
            // Engine
            {4, "Кэш дальних теней|Far shadow cache", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.gsmCache > 0 ? 1 : 0; },
             [] { const int d = g_giControl.gsmDefault; return d < 0 ? (g_giControl.gsmCache > 0 ? 1 : 0) : d; },
             [](int v) {
                 CryMTConsoleCommand(v ? "e_gsm_cache 1" : "e_gsm_cache 0"); // through the console: the engine's own handling
                 g_giControl.gsmCache = v;
             },
             true, [](bool ru) { return std::string(g_giControl.gsmCache < 0 ? "?" : g_giControl.gsmCache ? (ru ? "ВКЛ" : "ON") : (ru ? "ВЫКЛ" : "OFF")); }},
            {4, "DLAA в копиях кадра|DLAA into the frame's copies", 0, 1, 1, true, onOff, 0, nullptr,
             [] { return g_giControl.fixCopies ? 1 : 0; },
             [] {
                 wchar_t t[8] = {};
                 return GetEnvironmentVariableW(L"CRYMT_DLSS_FIXCOPIES", t, 8) && wcscmp(t, L"0") == 0 ? 0 : 1;
             },
             [](int v) { g_giControl.fixCopies = v; }, true, nullptr},
            {4, "Модель DLAA|DLAA model", 0, 5, 1, true, dlssModel, 0, nullptr,
             [] { const int v = g_giControl.dlssPreset; return v < 0 ? dlss::PresetDefaultIndex() : v; },
             [] { return dlss::PresetDefaultIndex(); }, [](int v) { g_giControl.dlssPreset = v; }, true, nullptr},
            // A second DLSS with another model on the right half (each half its own history; DLAA's cost twice).
            {4, "Сравнить модели (слева/справа)|Compare models (left/right)", 0, 6, 1, true, dlssCompare, 0, nullptr,
             [] { return g_giControl.dlssCompare + 1; }, [] { return 0; }, [](int v) { g_giControl.dlssCompare = v - 1; }, true, nullptr},
            // Time
            {5, "Время суток|Time of day", 0, 47, 2, true, nullptr, 0, nullptr,
             [] { const float t = g_giControl.timeOfDay; return t < 0 ? 0 : static_cast<int>(lroundf(t * 2.0f)) % 48; },
             [] { return static_cast<int>(lroundf(g_giControl.startTime * 2.0f)) % 48; },
             [](int v) { CryMTSetTimeOfDay(v * 0.5f); }, false,
             [](bool) {
                 const float t = g_giControl.timeOfDay;
                 if (t < 0)
                     return std::string("?");
                 const int minutes = static_cast<int>(t * 60.0f + 0.5f) % (24 * 60);
                 char b[16];
                 snprintf(b, sizeof(b), "%02d:%02d", minutes / 60, minutes % 60);
                 return std::string(b);
             }},
            {5, "Ускоренное время|Fast time", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.fastTime ? 1 : 0; },
             [] { return 0; }, [](int v) { g_giControl.fastTime = v != 0; }, true, nullptr},
        };
        return items;
    }

    // A key of the panel (main thread, CryMT.cpp's GiHotkeys).
    static void PanelKey(int action)
    {
        GiControl& c = g_giControl;
        if (action == kPanelShow)
        {
            c.panel = !c.panel;
            return;
        }
        if (!c.panel)
            return;
        const int tab = c.tab;
        if (action == kPanelTabPrev || action == kPanelTabNext)
        {
            c.tab = (tab + (action == kPanelTabNext ? 1 : kPanelTabs - 1)) % kPanelTabs;
            return;
        }
        std::vector<const PanelItem*> rows;
        for (const PanelItem& it : PanelItems())
            if (it.tab == tab)
                rows.push_back(&it);
        if (rows.empty())
            return;
        const int n = static_cast<int>(rows.size());
        int row = std::min(std::max(c.rows[tab].load(), 0), n - 1);
        if (action == kPanelUp || action == kPanelDown)
        {
            c.rows[tab] = (row + (action == kPanelDown ? 1 : n - 1)) % n;
            return;
        }
        const PanelItem& it = *rows[row];
        int v = it.get();
        if (action == kPanelDefault)
            v = it.def();
        else
        {
            v += action == kPanelMore ? it.step : -it.step;
            const int range = it.hi - it.lo + 1;
            v = it.wrap ? it.lo + ((v - it.lo) % range + range) % range : std::min(std::max(v, it.lo), it.hi);
        }
        it.set(v);
        Log("panel: %s = %d", PanelLang(it.label, false).c_str(), v);
    }

    static std::wstring Wide(const std::string& s)
    {
        std::wstring w(s.size(), L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], static_cast<int>(w.size()));
        w.resize(n > 0 ? n : 0);
        return w;
    }

    // The panel into its texture when it changed. False when there is none.
    bool UpdatePanel(UINT screenW, UINT screenH)
    {
        if (!g_giControl.active || !g_giControl.panel)
            return false;
        const bool ru = PanelRussian();
        const GiSettings& b = GiBase();
        const COLORREF white = RGB(225, 225, 225), dim = RGB(140, 150, 165), changed = RGB(255, 165, 60), title = RGB(250, 210, 90),
                       warn = RGB(240, 90, 80), barColour = RGB(28, 46, 78);
        struct Line
        {
            std::wstring left, right;
            COLORREF lc, rc;
            bool bar; // the selected row's background
        };
        std::vector<Line> lines;
        const int tab = std::min(std::max(g_giControl.tab.load(), 0), kPanelTabs - 1);
        if (g_giControl.numLockOff)
            lines.push_back({Wide(ru ? "NumLock выключен: включите его" : "NumLock is off: turn it on"), L"", warn, warn, false});
        // The tabs: the current one in brackets.
        {
            std::string t = "CryMT ";
            for (int i = 0; i < kPanelTabs; i++)
                t += (i == tab ? "[" : " ") + PanelLang(PanelTabName(i), ru) + (i == tab ? "]" : " ");
            lines.push_back({Wide(t), L"", title, title, false});
        }
        std::vector<const PanelItem*> rows;
        for (const PanelItem& it : PanelItems())
            if (it.tab == tab)
                rows.push_back(&it);
        const int selected = rows.empty() ? -1 : std::min(std::max(g_giControl.rows[tab].load(), 0), static_cast<int>(rows.size()) - 1);
        for (int i = 0; i < static_cast<int>(rows.size()); i++)
        {
            const PanelItem& it = *rows[i];
            const int v = it.get();
            std::string value;
            if (it.text)
                value = it.text(ru);
            else if (it.names)
                value = PanelLang(it.names[std::min(std::max(v, it.lo), it.hi) - it.lo], ru);
            else
            {
                char buf[32];
                snprintf(buf, sizeof(buf), it.format, v * it.scale);
                value = buf;
            }
            const bool off = it.mark && v != it.def();
            lines.push_back({Wide((i == selected ? "> " : "  ") + PanelLang(it.label, ru) + (off ? " *" : "")), Wide(value),
                             i == selected ? white : RGB(200, 200, 200), off ? changed : white, i == selected});
        }
        // Notes of the tab, then the keys.
        std::vector<std::string> notes;
        if (tab == 0 && !b.on)
            notes.push_back(ru ? "GI выключен в CryMT.ini (GI=1)" : "GI is off in CryMT.ini (GI=1)");
        if (tab == 3)
        {
            if (!b.on)
                notes.push_back(ru ? "нужен включённый GI" : "they need the GI on");
            notes.push_back(ru ? "не растительность и не террейн" : "not vegetation, terrain");
        }
        if (tab == 4)
            notes.push_back(ru ? "дальние каскады теней перерисовываются реже" : "far shadow cascades redrawn less often");
        if (tab == 5)
        {
            const int start = static_cast<int>(g_giControl.startTime * 60.0f + 0.5f) % (24 * 60);
            char buf[96];
            snprintf(buf, sizeof(buf), ru ? "Num5 на времени: %02d:%02d" : "Num5 on the time: %02d:%02d", start / 60, start % 60);
            notes.push_back(buf);
        }
        for (const std::string& n : notes)
            lines.push_back({Wide("  " + n), L"", dim, dim, false});
        lines.push_back({Wide(ru ? "Num7/9 вкладка  Num8/2 строка  Num4/6 значение" : "Num7/9 tab  Num8/2 row  Num4/6 value"), L"", dim,
                         dim, false});
        lines.push_back({Wide(ru ? "Num5 сброс  Num+/- время  Num/ скрыть" : "Num5 default  Num+/- time  Num/ hide"), L"", dim, dim, false});
        std::wstring text;
        for (const Line& l : lines)
            text += l.left + L"|" + l.right + (l.rc == changed ? L"*" : L"") + (l.bar ? L"#" : L"") + L"\n";
        text += std::to_wstring(screenW) + L"x" + std::to_wstring(screenH);
        if (text == m_panelText && m_panelTex)
            return true;
        m_panelText = text;
        const int px = static_cast<int>(screenH / 60 > 12 ? screenH / 60 : 12), lineH = px * 5 / 4, pad = px / 2;
        HDC dc = CreateCompatibleDC(nullptr);
        HFONT font = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ oldFont = SelectObject(dc, font);
        int leftW = 0, rightW = 0, wholeW = 0;
        for (const Line& l : lines)
        {
            SIZE s{};
            GetTextExtentPoint32W(dc, l.left.c_str(), static_cast<int>(l.left.size()), &s);
            if (l.right.empty())
                wholeW = std::max(wholeW, static_cast<int>(s.cx));
            else
            {
                leftW = std::max(leftW, static_cast<int>(s.cx));
                SIZE r{};
                GetTextExtentPoint32W(dc, l.right.c_str(), static_cast<int>(l.right.size()), &r);
                rightW = std::max(rightW, static_cast<int>(r.cx));
            }
        }
        const int gap = px * 2, rightX = pad + leftW + gap;
        const int w = std::max(rightX + rightW, pad + wholeW) + pad, h = static_cast<int>(lines.size()) * lineH + 2 * pad;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits)
        {
            SelectObject(dc, oldFont);
            DeleteObject(font);
            DeleteDC(dc);
            return false;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, static_cast<size_t>(w) * h * 4);
        SetBkMode(dc, TRANSPARENT);
        HBRUSH barBrush = CreateSolidBrush(barColour);
        for (size_t i = 0; i < lines.size(); i++)
        {
            const int y = pad + static_cast<int>(i) * lineH;
            const Line& l = lines[i];
            if (l.bar)
            {
                RECT r{pad / 2, y - (lineH - px) / 2, w - pad / 2, y - (lineH - px) / 2 + lineH};
                FillRect(dc, &r, barBrush);
            }
            SetTextColor(dc, l.lc);
            TextOutW(dc, pad, y, l.left.c_str(), static_cast<int>(l.left.size()));
            if (!l.right.empty())
            {
                SetTextColor(dc, l.rc);
                TextOutW(dc, rightX, y, l.right.c_str(), static_cast<int>(l.right.size()));
            }
        }
        DeleteObject(barBrush);
        GdiFlush();
        // Into a texture (B8G8R8A8, as GDI wrote it).
        if (!m_panelTex || m_panelW != static_cast<UINT>(w) || m_panelH != static_cast<UINT>(h))
        {
            DeferRelease(m_panelTex);
            m_panelTex = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = static_cast<UINT>(w);
            d.Height = static_cast<UINT>(h);
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            if (SUCCEEDED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                                                         reinterpret_cast<void**>(&m_panelTex))))
            {
                m_panelW = static_cast<UINT>(w);
                m_panelH = static_cast<UINT>(h);
                m_panelState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
        }
        bool ok = false;
        if (m_panelTex)
        {
            D3D12_RESOURCE_DESC rd = m_panelTex->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT64 total = 0;
            g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            ID3D12Resource* upload = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p = nullptr;
            if (upload && SUCCEEDED(upload->Map(0, nullptr, &p)))
            {
                for (int y = 0; y < h; y++)
                    memcpy(static_cast<uint8_t*>(p) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                           static_cast<const uint8_t*>(bits) + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
                upload->Unmap(0, nullptr);
                if (m_panelState != D3D12_RESOURCE_STATE_COPY_DEST)
                    m_barriers.push_back(Transition(m_panelTex, m_panelState, D3D12_RESOURCE_STATE_COPY_DEST));
                FlushBarriers();
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = m_panelTex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.pResource = upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fp;
                NoteCopy("panel", dst, 0, 0, 0, src, nullptr);
                m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                m_barriers.push_back(Transition(m_panelTex, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
                m_panelState = kSrvState;
                ok = true;
            }
            DeferRelease(upload);
        }
        SelectObject(dc, oldBmp);
        SelectObject(dc, oldFont);
        DeleteObject(bmp);
        DeleteObject(font);
        DeleteDC(dc);
        if (!ok)
            m_panelText.clear();
        return ok;
    }

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

    // The capture key's dumps (logs\play\<tag>_<name>.*): copies recorded where the textures' states are known (the
    // GI pass: the raw trace after the filter read it, the full-size GI after the upsample wrote it; the scene hand-over:
    // DLSS's input and output), read back once the frame capture has waited for the GPU anyway. Kinds: 0 HDR colour
    // (<name>.ppm with 1 - exp(-x), the engine's tone curve at exposure 1, and <name>.f16), 1 the full-size GI
    // (<name>_occ.ppm: a, the share of ambient removed, white = all; <name>_rgb.ppm: bounce x 64; <name>.f16), 2 the raw
    // trace (<name>_occ.ppm, thin features in red). .f16: width, height, RGBA half floats.
    static float Half(uint16_t h)
    {
        const uint32_t sgn = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
        uint32_t f;
        if (e == 0)
            f = m == 0 ? sgn << 31 : 0; // denormals as zero (tiny)
        else if (e == 31)
            f = (sgn << 31) | 0x7F800000u | (m << 13);
        else
            f = (sgn << 31) | ((e + 112u) << 23) | (m << 13);
        float r;
        memcpy(&r, &f, 4);
        return r;
    }

    struct PendingDump
    {
        ID3D12Resource* rb;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        std::string name;
        int kind;
    };
    std::vector<PendingDump> m_dumps;
    std::string m_dumpTag;

    // ---- Voxel snapshot (crymt_voxsnap <name>, measuring the grid's stability): right after a voxel update, levels
    // 0..kVoxGeomLevels-1 (the grid, the geometry stamps, the screen's "seen") read back; after the present's wait written
    // to logs\play\voxsnap_<name>.bin: "VXS1", n, levels, the GI frame, toWorld (16 floats), level 0's cell size, then per
    // level its first cell (3 ints, as the content stands) and n^3 texels (x fastest) of: opacity u8 (a x 255), light u8
    // (255 (1 - exp(-luminance))), the stamp u32 (frame + 1 << 8 | opacity byte), seen u8. tools\prof\voxsnap_diff.py.
    struct VoxSnapPart
    {
        ID3D12Resource* rb;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        int level, kind; // 0 the grid (RGBA16F), 1 the stamps (R32_UINT), 2 seen (R8_UINT)
    };
    std::vector<VoxSnapPart> m_voxSnap;
    std::string m_voxSnapName;
    float m_voxSnapToWorld[16] = {};
    int m_voxSnapGrid[kVoxGeomLevels][3] = {};
    uint64_t m_voxSnapFrame = 0;

    void RecordVoxSnap(const float* toWorld)
    {
        g_voxSnapWanted = false;
        {
            std::lock_guard<std::mutex> guard(g_voxSnapLock);
            m_voxSnapName = g_voxSnapName;
        }
        memcpy(m_voxSnapToWorld, toWorld, sizeof(m_voxSnapToWorld));
        m_voxSnapFrame = m_giFrame;
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            memcpy(m_voxSnapGrid[l], m_voxPrevGrid[l], sizeof(m_voxSnapGrid[l]));
            for (int kind = 0; kind < 3; kind++)
            {
                ID3D12Resource* res = kind == 0 ? m_vox[l] : kind == 1 ? m_voxOcc[l] : m_voxSeen[l];
                if (!res)
                    continue;
                const D3D12_RESOURCE_DESC rd = res->GetDesc();
                VoxSnapPart p{nullptr, {}, l, kind};
                UINT64 total = 0;
                g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &p.fp, nullptr, nullptr, &total);
                p.fp.Offset = 0;
                p.rb = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
                if (!p.rb)
                    continue;
                D3D12_RESOURCE_BARRIER b = Transition(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = p.rb;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = p.fp;
                src.pResource = res;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                NoteCopy("voxel snapshot", dst, 0, 0, 0, src, nullptr);
                m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                b = Transition(res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                m_list->ResourceBarrier(1, &b);
                m_voxSnap.push_back(p);
            }
        }
    }

    // After a wait for the GPU.
    void WriteVoxSnap()
    {
        const uint32_t n = kVoxN, levels = kVoxGeomLevels;
        std::string name;
        for (char ch : m_voxSnapName)
            name += isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' ? ch : '_';
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%svoxsnap_%s.bin", CryMTCapture("").c_str(), name.empty() ? "x" : name.c_str());
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") == 0 && f)
        {
            const float cell0 = Gi().voxelSize;
            fwrite("VXS1", 1, 4, f);
            fwrite(&n, 4, 1, f);
            fwrite(&levels, 4, 1, f);
            fwrite(&m_voxSnapFrame, 8, 1, f);
            fwrite(m_voxSnapToWorld, 4, 16, f);
            fwrite(&cell0, 4, 1, f);
            std::vector<uint8_t> a(n * n * n), lum(n * n * n), seen(n * n * n);
            std::vector<uint32_t> stamp(n * n * n);
            for (uint32_t l = 0; l < levels; l++)
            {
                std::fill(a.begin(), a.end(), 0);
                std::fill(lum.begin(), lum.end(), 0);
                std::fill(seen.begin(), seen.end(), 0);
                std::fill(stamp.begin(), stamp.end(), 0u);
                for (VoxSnapPart& p : m_voxSnap)
                {
                    void* mapped = nullptr;
                    if (p.level != static_cast<int>(l) || FAILED(p.rb->Map(0, nullptr, &mapped)) || !mapped)
                        continue;
                    const auto* base = static_cast<const uint8_t*>(mapped);
                    const size_t row = p.fp.Footprint.RowPitch, slice = row * p.fp.Footprint.Height;
                    for (uint32_t z = 0; z < n; z++)
                        for (uint32_t y = 0; y < n; y++)
                        {
                            const uint8_t* r = base + z * slice + y * row;
                            for (uint32_t x = 0; x < n; x++)
                            {
                                const size_t i = (static_cast<size_t>(z) * n + y) * n + x;
                                if (p.kind == 0)
                                {
                                    const auto* h = reinterpret_cast<const uint16_t*>(r + x * 8);
                                    const float alpha = std::min(std::max(Half(h[3]), 0.0f), 1.0f);
                                    const float y709 = 0.2126f * Half(h[0]) + 0.7152f * Half(h[1]) + 0.0722f * Half(h[2]);
                                    a[i] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
                                    lum[i] = static_cast<uint8_t>((1.0f - std::exp(-std::max(y709, 0.0f))) * 255.0f + 0.5f);
                                }
                                else if (p.kind == 1)
                                    stamp[i] = reinterpret_cast<const uint32_t*>(r)[x];
                                else
                                    seen[i] = r[x];
                            }
                        }
                    p.rb->Unmap(0, nullptr);
                }
                fwrite(m_voxSnapGrid[l], 4, 3, f);
                fwrite(a.data(), 1, a.size(), f);
                fwrite(lum.data(), 1, lum.size(), f);
                fwrite(stamp.data(), 4, stamp.size(), f);
                fwrite(seen.data(), 1, seen.size(), f);
            }
            fclose(f);
            Log("voxel snapshot %s: GI frame %llu, camera %.2f %.2f %.2f", path, static_cast<unsigned long long>(m_voxSnapFrame),
                m_voxSnapToWorld[12], m_voxSnapToWorld[13], m_voxSnapToWorld[14]);
        }
        else
            Log("voxel snapshot: cannot write %s", path);
        for (VoxSnapPart& p : m_voxSnap)
            DeferRelease(p.rb);
        m_voxSnap.clear();
    }
    bool m_giDumpWanted = false;

    // res is in COPY_SOURCE with the barriers flushed.
    // kind 6: an RGBK cube face (the engine's environment cube; sub = the face), written decoded as <name>.ppm.
    void RecordDump(ID3D12Resource* res, const char* name, int kind, UINT sub = 0)
    {
        if (!res)
            return;
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        const bool rg = rd.Format == DXGI_FORMAT_R16G16_FLOAT;
        const bool r8 = rd.Format == DXGI_FORMAT_R8_UNORM;
        const bool rgba8 = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const bool r16f = rd.Format == DXGI_FORMAT_R16_FLOAT;
        const bool r32f = rd.Format == DXGI_FORMAT_R32_FLOAT;
        const bool raw = kind == 11; // the texels as they are (GI v2's probe atlases)
        if (r16f && !raw)
            kind = 9; // one half per texel, raw (the DLSS bias mask)
        if (r32f && !raw)
            kind = 10; // one float per texel, raw (the GI's distances)
        if (rd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && rd.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS && !rg && !r8 && !rgba8 && !r16f &&
            !r32f)
        {
            Log("capture: %s format %d not dumped", name, static_cast<int>(rd.Format));
            return;
        }
        if (rg && !raw)
            kind = 3; // motion vectors
        if ((r8 || rgba8) && kind != 6 && !raw)
            kind = r8 ? 4 : 5; // one channel (the screen-space shadows; the shadow mask's red: the sun)
        PendingDump d{nullptr, {}, name, kind};
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&rd, sub, 1, 0, &d.fp, nullptr, nullptr, &total);
        d.fp.Offset = 0;
        d.rb = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!d.rb)
            return;
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = d.rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = d.fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = sub;
        NoteCopy("dump readback", dst, 0, 0, 0, src, nullptr);
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m_dumps.push_back(d);
    }

    // After a wait for the GPU (the frame capture's WaitIdle).
    void WriteDumps()
    {
        for (PendingDump& d : m_dumps)
        {
            void* p = nullptr;
            if (d.kind == 6 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // An RGBK cube face: rgb a 16 (HDRDecode_FromLDR_RGBK), shown as 1 - exp(-x); mean HDR and alpha in the log.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.ppm", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                fopen_s(&f, path, "wb");
                if (f)
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
                const int faceIndex = d.name.size() == 8 && d.name.compare(0, 7, "envcube") == 0 ? d.name[7] - '0' : -1;
                if (faceIndex >= 0 && faceIndex < 6)
                {
                    m_cubeFaces[faceIndex].assign(static_cast<size_t>(w) * h * 3, 0.0f);
                    m_cubeFaceSize = w;
                }
                double sum[4] = {};
                for (UINT y = 0; y < h; y++)
                {
                    const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch;
                    for (UINT x = 0; x < w; x++)
                    {
                        const float k = src[x * 4 + 3] / 255.0f * 16.0f;
                        for (int c = 0; c < 3; c++)
                        {
                            const float v = src[x * 4 + c] / 255.0f * k;
                            if (faceIndex >= 0 && faceIndex < 6 && w == h)
                                m_cubeFaces[faceIndex][(static_cast<size_t>(y) * w + x) * 3 + c] = v;
                            sum[c] += v;
                            row[x * 3 + c] = static_cast<uint8_t>(std::min(1.0f - std::exp(-v), 1.0f) * 255.0f);
                        }
                        sum[3] += src[x * 4 + 3] / 255.0;
                    }
                    if (f)
                        fwrite(row.data(), 1, row.size(), f);
                }
                if (f)
                    fclose(f);
                const double n = double(w) * h;
                Log("capture: %s mean HDR %.3f %.3f %.3f, alpha %.3f (%ux%u)", d.name.c_str(), sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, w, h);
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if ((d.kind == 4 || d.kind == 5) && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // One 8-bit channel: <name>.pgm (white = 1), its mean and the share over 0.5 in the log.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height, step = d.kind == 4 ? 1 : 4;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.pgm", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                fopen_s(&f, path, "wb");
                if (f)
                    fprintf(f, "P5\n%u %u\n255\n", w, h);
                std::vector<uint8_t> row(w);
                double sum = 0;
                uint64_t over = 0;
                for (UINT y = 0; y < h; y++)
                {
                    const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch;
                    for (UINT x = 0; x < w; x++)
                    {
                        row[x] = src[x * step];
                        sum += row[x];
                        over += row[x] > 127;
                    }
                    if (f)
                        fwrite(row.data(), 1, w, f);
                }
                if (f)
                    fclose(f);
                Log("capture: %s mean %.4f, over 0.5 %.2f%% (%ux%u)", d.name.c_str(), sum / 255.0 / (double(w) * h), 100.0 * over / (double(w) * h), w, h);
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 11 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // Raw texels: <name>.raw (width, height, bytes a texel, then the rows), e.g. GI v2's probe atlases.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                const UINT bpp = d.fp.Footprint.Format == DXGI_FORMAT_R16G16_FLOAT ? 4 : 8;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.raw", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                if (fopen_s(&f, path, "wb") == 0 && f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                    fwrite(&bpp, 4, 1, f);
                    for (UINT y = 0; y < h; y++)
                        fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * bpp, f);
                    fclose(f);
                }
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 10 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // One channel of floats: <name>.r32 (width, height, then the rows).
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.r32", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                if (fopen_s(&f, path, "wb") == 0 && f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                    for (UINT y = 0; y < h; y++)
                        fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * 4, f);
                    fclose(f);
                }
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 9 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // One channel of halves: <name>.f16 (width, height, then the rows).
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.f16", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                if (fopen_s(&f, path, "wb") == 0 && f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                    for (UINT y = 0; y < h; y++)
                        fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * 2, f);
                    fclose(f);
                }
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 3 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // Motion vectors (R16G16F, UV units): <name>.f16 as RG halves, and the mean in pixels in the log.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                double sx = 0, sy = 0;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.f16", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                fopen_s(&f, path, "wb");
                if (f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                }
                for (UINT y = 0; y < h; y++)
                {
                    const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch);
                    if (f)
                        fwrite(row, 1, static_cast<size_t>(w) * 4, f);
                    for (UINT x = 0; x < w; x++)
                    {
                        sx += Half(row[x * 2]);
                        sy += Half(row[x * 2 + 1]);
                    }
                }
                if (f)
                    fclose(f);
                Log("capture: %s mean motion %.3f, %.3f px", d.name.c_str(), sx / (double(w) * h) * w, sy / (double(w) * h) * h);
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                const auto at = [&](UINT x, UINT y, int c) {
                    return Half(*reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch + x * 8 + c * 2));
                };
                const auto ppm = [&](const char* suffix, auto pixel) {
                    char path[MAX_PATH];
                    snprintf(path, sizeof(path), "%s%s_%s%s.ppm", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str(), suffix);
                    FILE* f = nullptr;
                    if (fopen_s(&f, path, "wb") != 0 || !f)
                        return;
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                    std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
                    for (UINT y = 0; y < h; y++)
                    {
                        for (UINT x = 0; x < w; x++)
                            pixel(x, y, &row[x * 3]);
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                };
                const auto byte = [](float v) { return static_cast<uint8_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f); };
                if (d.kind == 7)
                {
                    // raw only (the motion blur target of a motion vector sequence)
                }
                else if (d.kind == 0)
                    ppm("", [&](UINT x, UINT y, uint8_t* o) {
                        for (int c = 0; c < 3; c++)
                        {
                            const float v = at(x, y, c);
                            o[c] = v == v ? byte(1.0f - std::exp(-v)) : 255;
                        }
                    });
                else
                    ppm("_occ", [&](UINT x, UINT y, uint8_t* o) {
                        const float a = at(x, y, 3);
                        const bool thin = a < -0.5f;
                        const uint8_t v = byte(thin ? -1.0f - a : a);
                        o[0] = thin ? 255 : v;
                        o[1] = thin ? 0 : v;
                        o[2] = thin ? 0 : v;
                    });
                if (d.kind == 1)
                    ppm("_rgb", [&](UINT x, UINT y, uint8_t* o) {
                        for (int c = 0; c < 3; c++)
                            o[c] = byte(at(x, y, c) * 0.25f); // bounce x 64 (of 255)
                    });
                if (d.kind != 2)
                {
                    char path[MAX_PATH];
                    snprintf(path, sizeof(path), "%s%s_%s.f16", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                    FILE* f = nullptr;
                    if (fopen_s(&f, path, "wb") == 0 && f)
                    {
                        fwrite(&w, 4, 1, f);
                        fwrite(&h, 4, 1, f);
                        for (UINT y = 0; y < h; y++)
                            fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * 8, f);
                        fclose(f);
                    }
                }
                d.rb->Unmap(0, nullptr);
            }
            d.rb->Release();
        }
        if (!m_dumps.empty())
            Log("capture: %zu dumps written (%s)", m_dumps.size(), m_dumpTag.c_str());
        m_dumps.clear();
        WriteCubePicture();
    }

    // The engine's cube as one picture (<tag>_envcube.bmp, Windows opens it): what one standing at the cube's place sees
    // looking +X, -X, +Y (top row), -Y, up, down (bottom row), each labelled; the sides with Z up (horizon level, sky on
    // top), up and down with +Y at the top. Every pixel is the cube looked up by its world direction as the water does
    // (D3D cube addressing), so a face stored turned or mirrored shows so. Exposure from the cube's log-average (0.18,
    // Reinhard, gamma 2.2), as a photo; the HDR is RGBK-decoded.
    std::vector<float> m_cubeFaces[6];
    UINT m_cubeFaceSize = 0;
    void WriteCubePicture()
    {
        const UINT s = m_cubeFaceSize;
        for (auto& face : m_cubeFaces)
            if (!s || face.size() != static_cast<size_t>(s) * s * 3)
                return;
        static const char* const kGlyphs[5][7] = {
            {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}, // +
            {".....", ".....", ".....", "#####", ".....", ".....", "....."}, // -
            {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}, // X
            {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}, // Y
            {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}, // Z
        };
        const UINT gap = 8, W = s * 3 + gap * 4, H = s * 2 + gap * 3, scale = std::max(2u, s / 64);
        std::vector<uint8_t> img(static_cast<size_t>(W) * H * 3, 40);
        double logSum = 0;
        for (const auto& face : m_cubeFaces)
            for (size_t i = 0; i < face.size(); i += 3)
                logSum += std::log(1e-4 + 0.2126 * face[i] + 0.7152 * face[i + 1] + 0.0722 * face[i + 2]);
        const float exposure = static_cast<float>(0.18 / std::exp(logSum / (6.0 * s * s)));
        // Per panel: forward and up (right = forward x up).
        static const float kView[6][6] = {{1, 0, 0, 0, 0, 1}, {-1, 0, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, {0, -1, 0, 0, 0, 1}, {0, 0, 1, 0, 1, 0}, {0, 0, -1, 0, 1, 0}};
        for (int f = 0; f < 6; f++)
        {
            const UINT ox = gap + (f % 3) * (s + gap), oy = gap + (f / 3) * (s + gap);
            const float* fw = kView[f];
            const float* up = kView[f] + 3;
            const float rt[3] = {fw[1] * up[2] - fw[2] * up[1], fw[2] * up[0] - fw[0] * up[2], fw[0] * up[1] - fw[1] * up[0]};
            for (UINT y = 0; y < s; y++)
                for (UINT x = 0; x < s; x++)
                {
                    const float a = 2.0f * (x + 0.5f) / s - 1.0f, bb = 1.0f - 2.0f * (y + 0.5f) / s;
                    float d[3] = {fw[0] + a * rt[0] + bb * up[0], fw[1] + a * rt[1] + bb * up[1], fw[2] + a * rt[2] + bb * up[2]};
                    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
                    if (ay >= ax && ay >= az) // the engine's faces are flipped vertically (as Water.cfx looks them up)
                        d[2] = -d[2];
                    else
                        d[1] = -d[1];
                    int face;
                    float sc, tc;
                    if (ax >= ay && ax >= az)
                        face = d[0] > 0 ? 0 : 1, sc = (d[0] > 0 ? -d[2] : d[2]) / ax, tc = -d[1] / ax;
                    else if (ay >= az)
                        face = d[1] > 0 ? 2 : 3, sc = d[0] / ay, tc = (d[1] > 0 ? d[2] : -d[2]) / ay;
                    else
                        face = d[2] > 0 ? 4 : 5, sc = (d[2] > 0 ? d[0] : -d[0]) / az, tc = -d[1] / az;
                    const float wide = g_engineCubeTanHalf.load(); // the engine's faces see wider than 90 degrees
                    const UINT u = std::min(s - 1, static_cast<UINT>(std::max(0.0f, (sc / wide + 1) * 0.5f * s)));
                    const UINT v = std::min(s - 1, static_cast<UINT>(std::max(0.0f, (tc / wide + 1) * 0.5f * s)));
                    const float* hdr = &m_cubeFaces[face][(static_cast<size_t>(v) * s + u) * 3];
                    uint8_t* p = &img[(static_cast<size_t>(oy + y) * W + ox + x) * 3];
                    for (int c = 0; c < 3; c++)
                    {
                        const float e = hdr[c] * exposure;
                        p[c] = static_cast<uint8_t>(255.0f * std::pow(e / (1.0f + e), 1.0f / 2.2f));
                    }
                }
            const int glyphs[2] = {f % 2 == 0 ? 0 : 1, 2 + f / 2};
            for (int g = 0; g < 2; g++)
                for (UINT gy = 0; gy < 9 * scale; gy++)
                    for (UINT gx = 0; gx < 7 * scale; gx++)
                    {
                        const int cx = static_cast<int>(gx / scale) - 1, cy = static_cast<int>(gy / scale) - 1;
                        const bool on = cx >= 0 && cx < 5 && cy >= 0 && cy < 7 && kGlyphs[glyphs[g]][cy][cx] == '#';
                        uint8_t* p = &img[(static_cast<size_t>(oy + 4 + gy) * W + ox + 4 + g * 7 * scale + gx) * 3];
                        p[0] = on ? 255 : 0; // RGB here, BGR on writing
                        p[1] = on ? 255 : 0;
                        p[2] = on ? 0 : 0;
                    }
            m_cubeFaces[f].clear();
        }
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s%s_envcube.bmp", CryMTCapture("").c_str(), m_dumpTag.c_str());
        FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") != 0 || !file)
            return;
        const UINT rowBytes = (W * 3 + 3) & ~3u, size = 54 + rowBytes * H;
        uint8_t header[54] = {'B', 'M'};
        const auto put = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
        put(2, size);
        put(10, 54);
        put(14, 40);
        put(18, W);
        put(22, H);
        header[26] = 1;
        header[28] = 24;
        put(34, rowBytes * H);
        fwrite(header, 1, 54, file);
        std::vector<uint8_t> row(rowBytes, 0);
        for (UINT y = H; y-- > 0;)
        {
            for (UINT x = 0; x < W; x++)
            {
                const uint8_t* p = &img[(static_cast<size_t>(y) * W + x) * 3];
                row[x * 3] = p[2];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = p[0];
            }
            fwrite(row.data(), 1, rowBytes, file);
        }
        fclose(file);
        Log("engine cube picture: %s", path);
    }

    // DLAA output (the color's format, unordered access) and the motion vectors (RunMotion; zero until it runs), made
    // when the scene size changes.
    bool EnsureDlssTargets(UINT width, UINT height, DXGI_FORMAT format)
    {
        if (m_dlssOut && width == m_dlssWidth && height == m_dlssHeight && format == m_dlssFormat)
            return true;
        DeferRelease(m_dlssOut);
        DeferRelease(m_dlssMvec);
        DeferRelease(m_dlssIn);
        DeferRelease(m_dlssOut2);
        m_dlssOut = m_dlssMvec = m_dlssIn = m_dlssOut2 = nullptr;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                    reinterpret_cast<void**>(&m_dlssOut));
        if (SUCCEEDED(hr))
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_dlssIn));
        d.Format = DXGI_FORMAT_R16G16_FLOAT;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_CLEAR_VALUE zero{};
        zero.Format = DXGI_FORMAT_R16G16_FLOAT;
        if (SUCCEEDED(hr))
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &zero, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_dlssMvec));
        if (FAILED(hr))
        {
            Log("DLSS targets %ux%u failed %08lx", width, height, static_cast<unsigned long>(hr));
            return false;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvCpu.Alloc();
        g.dev->CreateRenderTargetView(m_dlssMvec, nullptr, rtv);
        const float clear[4] = {};
        m_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        g_rtvCpu.Free(rtv); // read when the clear was recorded
        m_barriers.push_back(Transition(m_dlssMvec, D3D12_RESOURCE_STATE_RENDER_TARGET, kSrvState));
        m_dlssWidth = width;
        m_dlssHeight = height;
        m_dlssFormat = format;
        return true;
    }

    // DLSS motion vectors (the shader's motion entry): the camera motion of every point from depth and this and the
    // previous scene's camera, plus the engine's object velocities when its motion blur pass drew them this frame.
    // DLSS's exposure (the shader's exposure entry), left for it in shader-resource state; only with CRYMT_DLSS_EXPOSURE=1,
    // else DLSS's own auto exposure. CRYMT_DLSS_EXPOSUREKEY: the key (0.18), the scene's average luminance times the
    // exposure. Island 4:30 (night shade sparkles under DLAA): ~215 a frame either way, so off by default.
    bool RunExposure(TextureCore* color, bool reset)
    {
        static const float key = [] {
            char text[32] = {};
            if (!GetEnvironmentVariableA("CRYMT_DLSS_EXPOSURE", text, sizeof(text)) || strcmp(text, "0") == 0)
                return 0.0f;
            return GetEnvironmentVariableA("CRYMT_DLSS_EXPOSUREKEY", text, sizeof(text)) ? static_cast<float>(atof(text)) : 0.18f;
        }();
        if (key <= 0 || !EnsureGiPipeline() || !m_expPso)
            return false;
        if (!m_dlssExposure)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_dlssExposure))))
            {
                m_dlssExposure = nullptr;
                return false;
            }
            reset = true;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(6, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        Require(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FlushBarriers();
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = color->format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT : color->format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        for (int i = 0; i < 3; i++)
            g.dev->CreateShaderResourceView(color->res, &sd, at(i)); // t1 read
        for (int i = 3; i < 6; i++)
            g.dev->CreateUnorderedAccessView(m_dlssExposure, nullptr, nullptr, at(i)); // u1 read and written
        float c[kGiConstants] = {};
        c[8] = key;                 // radius
        c[9] = 0.05f;               // bounce: the share of the way to the new value per frame
        c[10] = 0.05f;              // occlusion: the least exposure
        c[11] = reset ? 1.0f : 0.0f; // temporal
        c[12] = 1000.0f;            // maxDistance: the most
        m_list->SetComputeRootSignature(m_giRoot);
        m_list->SetComputeRoot32BitConstants(0, kGiConstants, c, 0);
        m_list->SetPipelineState(m_expPso);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->Dispatch(1, 1, 1);
        m_barriers.push_back(Transition(m_dlssExposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrvState));
        RestoreListState();
        return true;
    }

    // CRYMT_DLSS_CLAMP (tests): 1 or in: DLSS reads a copy of the colour without negative, NaN or infinite components;
    // out: DLSS's output is cleaned the same way in place; both.
    static int DlssClampMode()
    {
        static const int mode = [] {
            char text[8] = {};
            if (!GetEnvironmentVariableA("CRYMT_DLSS_CLAMP", text, sizeof(text)) || strcmp(text, "0") == 0)
                return 0;
            return strcmp(text, "out") == 0 ? 2 : strcmp(text, "both") == 0 ? 3 : 1;
        }();
        return mode;
    }

    void RunSanitizeOut(UINT width, UINT height)
    {
        if (!(DlssClampMode() & 2) || !m_dlssOut || !EnsureGiPipeline() || !m_sanOutPso)
            return;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(6, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        for (int i = 0; i < 3; i++)
            g.dev->CreateShaderResourceView(m_dlssOut, nullptr, at(i)); // not read
        for (int i = 3; i < 6; i++)
            g.dev->CreateUnorderedAccessView(m_dlssOut, nullptr, nullptr, at(i)); // u0 read and written
        float c[kGiConstants] = {static_cast<float>(width), static_cast<float>(height)};
        m_list->SetComputeRootSignature(m_giRoot);
        m_list->SetComputeRoot32BitConstants(0, kGiConstants, c, 0);
        m_list->SetPipelineState(m_sanOutPso);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = m_dlssOut;
        m_list->ResourceBarrier(1, &uav);
        RestoreListState();
    }

    bool RunSanitize(TextureCore* color)
    {
        if (!(DlssClampMode() & 1) || !m_dlssIn || !EnsureGiPipeline() || !m_sanPso)
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(6, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        Require(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FlushBarriers();
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = color->format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT : color->format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        for (int i = 0; i < 3; i++)
            g.dev->CreateShaderResourceView(color->res, &sd, at(i)); // t1 read
        for (int i = 3; i < 6; i++)
            g.dev->CreateUnorderedAccessView(m_dlssIn, nullptr, nullptr, at(i)); // u0 written
        float c[kGiConstants] = {static_cast<float>(color->width), static_cast<float>(color->height)};
        m_list->SetComputeRootSignature(m_giRoot);
        m_list->SetComputeRoot32BitConstants(0, kGiConstants, c, 0);
        m_list->SetPipelineState(m_sanPso);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->Dispatch((color->width + 7) / 8, (color->height + 7) / 8, 1);
        m_barriers.push_back(Transition(m_dlssIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrvState));
        RestoreListState();
        return true;
    }

    bool RunMotion(const DlssSceneInfo& info, TextureCore* color, TextureCore* depth)
    {
        static const bool off = [] {
            wchar_t text[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_MOTION", text, 8) && wcscmp(text, L"0") == 0; // Streamline's camera motion
        }();
        if (off || !EnsureGiPipeline() || !m_mvPso)
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(6, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        KeepObjectMotion(); // a pass with nothing drawn after it
        TextureCore* velocity = m_ombCopyFrame == m_frame && g_objectMotion ? &m_ombCopy : nullptr;
        // CryMT's exact object motion (BindObjectMotion) when the patched pass drew this frame; the engine's target else.
        Srv12* exact = g_objectMotion && m_objMvSrv && m_objMvFrame == m_frame && m_objMvW == color->width && m_objMvH == color->height &&
                               m_objMvSrv->info ? m_objMvSrv : nullptr;
        Srv12* bend = g_objectMotion && m_bendSrv && m_bendFrame == m_frame && m_bendW == color->width && m_bendH == color->height ? m_bendSrv : nullptr;
        if (velocity && m_ombDiagFrame == m_frame && m_frame > 1500 && m_ombDiagLogged < 1)
        {
            m_ombDiagLogged++;
            Log("motion blur draws, frame %llu: %u (scene colour %p res %p %ux%u)", static_cast<unsigned long long>(m_frame), m_ombDiagCount,
                static_cast<void*>(color), static_cast<void*>(color->res), color->width, color->height);
            for (uint32_t i = 0; i < std::min(m_ombDiagCount, 8u); i++)
            {
                const OmbDiag& o = m_ombDiag[i];
                Log("  target %p res %p %ux%u format %d%s, vs %016llx ps %016llx", static_cast<void*>(o.target), static_cast<void*>(o.target->res),
                    o.target->width, o.target->height, static_cast<int>(o.target->format), o.target->res == color->res ? " (the scene colour)" : "",
                    static_cast<unsigned long long>(o.vs), static_cast<unsigned long long>(o.ps));
            }
        }
        if (velocity && (velocity == color || velocity->width != color->width || velocity->height != color->height ||
                         velocity->format != DXGI_FORMAT_R16G16B16A16_FLOAT || velocity->samples != 1))
            velocity = nullptr;
        if (exact)
            velocity = nullptr;
        Require(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (exact)
            Require(exact->info, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (velocity)
            Require(velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (bend)
            Require(bend->info, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_barriers.push_back(Transition(m_dlssMvec, kSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        FlushBarriers();
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DepthSrvFormat(depth->format);
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(depth->res, &sd, at(0));
        if (exact)
            g.dev->CreateShaderResourceView(exact->info->res, nullptr, at(1));
        else if (velocity)
            g.dev->CreateShaderResourceView(velocity->res, nullptr, at(1));
        else
            g.dev->CreateShaderResourceView(depth->res, &sd, at(1)); // not read
        if (bend)
            g.dev->CreateShaderResourceView(bend->info->res, nullptr, at(2));
        else
            g.dev->CreateShaderResourceView(depth->res, &sd, at(2)); // not read
        g.dev->CreateUnorderedAccessView(m_dlssMvec, nullptr, nullptr, at(3));
        g.dev->CreateUnorderedAccessView(m_dlssMvec, nullptr, nullptr, at(4)); // not written
        g.dev->CreateUnorderedAccessView(m_dlssMvec, nullptr, nullptr, at(5)); // not written
        // This view to the world, then the previous view and projection; a cut (or the first frame) has no motion.
        float toWorld[16], reproj[16];
        RigidInverse(info.view, toWorld);
        // A cut: more than 10 m or a turn of more than 45 degrees in one frame (2026-11-13: a cutscene cut that only turns the
        // camera gave vectors of thousands of pixels).
        if (info.reset || !m_mvHavePrev || CameraJump(info.view, info.camPos, m_mvPrevView, m_mvPrevCamPos) > 10.0 ||
            ViewTurnCos(info.view, m_mvPrevView) < 0.7071)
        {
            memcpy(m_mvPrevView, info.view, sizeof(m_mvPrevView));
            memcpy(m_mvPrevProj, info.proj, sizeof(m_mvPrevProj));
            memcpy(m_mvPrevCamPos, info.camPos, sizeof(m_mvPrevCamPos));
        }
        ReprojDouble(info.view, info.camPos, m_mvPrevView, m_mvPrevCamPos, m_mvPrevProj, reproj);
        float c[kGiConstants] = {static_cast<float>(color->width), static_cast<float>(color->height), 0.0f, 0.0f,
                                 1.0f / info.proj[0], 1.0f / info.proj[5], info.proj[10], info.proj[14]};
        memcpy(c + 16, reproj, sizeof(reproj));
        c[8] = zpass::MotionBlurShutter(); // radius: the engine's motion blur shutter factor f this frame
        c[32] = info.farZ;
        c[33] = exact ? 2.0f : velocity ? 1.0f : 0.0f;
        // The bent vegetation's move is per 1/60 s: scaled to the time since the last frame (the game timer's, as PF_time).
        float dt = info.frameTime;
        if (m_mvHavePrevTime && info.time > m_mvPrevTime)
            dt = info.time - m_mvPrevTime;
        m_mvPrevTime = info.time;
        m_mvHavePrevTime = info.time > 0;
        c[34] = std::min(std::max(dt, 0.0f), 0.1f) * 60.0f;
        c[35] = bend ? 1.0f : 0.0f;
        memcpy(m_mvPrevView, info.view, sizeof(m_mvPrevView));
        memcpy(m_mvPrevProj, info.proj, sizeof(m_mvPrevProj));
        memcpy(m_mvPrevCamPos, info.camPos, sizeof(m_mvPrevCamPos));
        memcpy(m_mvPrevToWorld, toWorld, sizeof(m_mvPrevToWorld));
        m_mvHavePrev = true;
        m_list->SetComputeRootSignature(m_giRoot);
        m_list->SetComputeRoot32BitConstants(0, kGiConstants, c, 0);
        m_list->SetPipelineState(m_mvPso);
        m_list->SetComputeRootDescriptorTable(1, gpu);
        m_list->Dispatch((color->width + 7) / 8, (color->height + 7) / 8, 1);
        m_barriers.push_back(Transition(m_dlssMvec, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrvState));
        m_stats.motionPasses++;
        if (velocity || exact)
            m_stats.objectMotionPasses++;
        if (exact)
            m_stats.objectMotionExactPasses++;
        if (bend)
            m_stats.bendMotionPasses++;
        RestoreListState();
        return true;
    }

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

// What a resource's GetDevice gives: the device the engine holds (D3DLayer's wrapper), not this back end. D3DX10 takes the
// device from a texture: the save thumbnail's resize (CryRenderD3D10+0xdbfda, on main in a sync) called this back end's
// CopySubresourceRegion directly while the stream thread recorded into the same command list - the driver crashed
// (nvwgf2umx+0x6dbaf5, 2026-11-12 15:51 and 19:24) or the GPU hung (driver resets at saves). Through the wrapper the
// calls are recorded into the command stream in order, as the engine's own calls in the same function are.
ID3D10Device* DeviceInterface()
{
    if (ID3D10Device* pub = ::D3DLayerPublicDevice())
    {
        pub->AddRef();
        return pub;
    }
    g_device->AddRef();
    return g_device;
}

void SubmitMain()
{
    g_device->Submit();
}

void PollDone()
{
    std::lock_guard<std::recursive_mutex> guard(g.lock);
    Retire();
}

bool Device::Init()
{
    m_frameRing.Init(64u << 20);
    m_srvRing.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 500000);
    m_samplerHeap.Init(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048);
    m_samplerHeap.size = 2048 - kTableSamplers; // a table's 16-entry root range stays inside the heap
    m_zeroCb = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, 65536, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_frameRing.res || !m_srvRing.heap || !m_samplerHeap.heap || !m_zeroCb)
        return false;
    memset(MapWhole(m_zeroCb), 0, 65536);
    m_nullRtv = g_rtvCpu.Alloc();
    D3D12_RENDER_TARGET_VIEW_DESC nullRtv{};
    nullRtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.dev->CreateRenderTargetView(nullptr, &nullRtv, m_nullRtv);
    m_barriers.reserve(64);
    D3D12_QUERY_HEAP_DESC stamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kListStamps, 0};
    g.dev->CreateQueryHeap(&stamps, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_listStampHeap));
    m_listStampReadback = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, 2 * kListStamps * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC giStamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kGiStamps * kGiStampRuns, 0};
    g.dev->CreateQueryHeap(&giStamps, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_giStampHeap));
    m_giStampReadback = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, kGiStamps * kGiStampRuns * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST);
    if (m_giStampHeap && m_giStampReadback)
    {
        void* p = nullptr;
        m_giStampReadback->Map(0, nullptr, &p);
        m_giStamps = static_cast<const uint64_t*>(p);
    }
    if (m_listStampHeap && m_listStampReadback)
    {
        void* p = nullptr;
        m_listStampReadback->Map(0, nullptr, &p);
        m_listStamps = static_cast<const uint64_t*>(p);
    }
    if (m_passNamesOn && GetEnvironmentVariableW(L"CRYMT_GPUPASSES", nullptr, 0))
    {
        D3D12_QUERY_HEAP_DESC passStamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kPassStamps * kPassRing, 0};
        g.dev->CreateQueryHeap(&passStamps, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_passStampHeap));
        m_passStampReadback = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, kPassStamps * kPassRing * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST);
        if (m_passStampHeap && m_passStampReadback)
        {
            void* p = nullptr;
            m_passStampReadback->Map(0, nullptr, &p);
            m_passStampCpu = static_cast<const uint64_t*>(p);
            m_passStampsOn = m_passStampCpu != nullptr;
        }
        Log("GPU time per pass: %s", m_passStampsOn ? "on" : "failed");
    }
    m_alloc = m_allocs.Get();
    if (FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                                        reinterpret_cast<void**>(&m_list))))
        return false;
    m_list->Close();
    m_allocs.Put(m_alloc, 0);
    BeginList();
    // CRYMT_GITEST: a magenta 1x1 GI texture with full weight in place of the placeholder, to check the path into the
    // shaders' ambient term.
    if (GetEnvironmentVariableW(L"CRYMT_GITEST", nullptr, 0))
    {
        static const uint16_t kMagenta[4] = {0x4400, 0, 0x4400, 0x3C00}; // half floats 4, 0, 4, 1: HDR magenta over the sun
        D3D10_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D10_USAGE_DEFAULT;
        td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        const D3D10_SUBRESOURCE_DATA data{kMagenta, 8, 8};
        ID3D10Texture2D* tex = nullptr;
        ID3D10ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(CreateTexture2D(&td, &data, &tex)) && SUCCEEDED(CreateShaderResourceView(tex, nullptr, &srv)))
        {
            m_giSrv = static_cast<Srv12*>(srv);
            m_giCpu = m_giSrv->cpu;
        }
        Log("GI test texture: %s", m_giSrv ? "ready" : "failed");
    }
    return true;
}

void Device::BeginList()
{
    m_alloc = m_allocs.Get();
    m_list->Reset(m_alloc, nullptr);
    RestoreListState();
    for (const std::wstring& name : m_passNames)
        m_list->BeginEvent(0, name.c_str(), static_cast<UINT>((name.size() + 1) * sizeof(wchar_t)));
    m_drawsSinceSubmit = 0;
    if (m_listStamps)
        m_list->EndQuery(m_listStampHeap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(g_mainSerial % kListStamps) * 2);
    PassStamp(PassLabel());
}

// Everything the list needs set again: after a reset, or after Streamline recorded into it.
void Device::RestoreListState()
{
    ID3D12DescriptorHeap* heaps[] = {m_srvRing.heap, m_samplerHeap.heap};
    m_list->SetDescriptorHeaps(2, heaps);
    m_list->SetGraphicsRootSignature(g.root);
    m_psoDirty = true;
    m_psoApplied = nullptr;
    m_topologyDirty = m_omDirty = m_viewportDirty = m_scissorDirty = m_blendFactorDirty = m_stencilRefDirty = true;
    memset(m_vbApplied, 0, sizeof(m_vbApplied));
    m_ibApplied = {};
    for (StageState& st : m_stage)
    {
        memset(st.cbApplied, 0, sizeof(st.cbApplied));
        st.srvDirty = st.samplerDirty = true;
        st.srvApplied = st.samplerApplied = 0;
    }
}

void Device::PassName(const char* text)
{
    // CryMT.cpp EnvCubeUpdate marks the engine's cube render in stream order (with or without pass names).
    // "CryMT engine cube" (a face mixed in), "... fresh" (it replaces the old one), "... end" (the HDR copy takes it).
    // A start mark may end in " f<n>": the engine's face this render draws (the culling per face).
    const char* cubeRest = strncmp(text, "CryMT engine cube", 17) == 0 ? text + 17 : nullptr;
    const char* cubeF = cubeRest ? strrchr(cubeRest, 'f') : nullptr;
    const int cubeFace = cubeF && cubeF > cubeRest && cubeF[-1] == ' ' && cubeF[1] >= '0' && cubeF[1] <= '5' && !cubeF[2] ? cubeF[1] - '0' : -1;
    const size_t cubeBase = cubeRest ? (cubeFace >= 0 ? static_cast<size_t>(cubeF - 1 - cubeRest) : strlen(cubeRest)) : 0;
    const bool cubeFreshMark = cubeRest && cubeBase == 6 && !strncmp(cubeRest, " fresh", 6);
    if (cubeRest && (cubeBase == 0 || cubeFreshMark || !strcmp(cubeRest, " end")))
    {
        const bool end = !strcmp(cubeRest, " end");
        if (!end)
        {
            m_engineCubeFresh = cubeFreshMark;
            m_engineCubeLastFace = -1;
            m_engineCubeFace = cubeFace;
        }
        m_engineCubePass = !end;
        m_psoDirty = true; // its culling (EngineCubeCull)
        // The cube's slice the render ended in, for the face number its start mark gave: the culling per face goes by the
        // slice (EngineCubeSlice).
        if (end && m_engineCubeFace >= 0 && m_engineCubeLastFace >= 0)
        {
            if (g_cubeSliceOfMark[m_engineCubeFace] != m_engineCubeLastFace)
                g_cubeSliceLearned++;
            g_cubeSliceOfMark[m_engineCubeFace] = m_engineCubeLastFace;
        }
        if (end && m_engineCubeLastFace >= 0 && (EngineCubeOn() || EnvEngineOn()))
        {
            PassName("CryMT cube HDR");
            RunCubeUpdate(m_engineCubeLastFace, m_engineCubeFresh);
            PassNameEnd();
        }
        // The face's solid draws into the GI's geometry levels now, while their buffers hold what the face drew.
        if (end && !m_cubeDraws.empty())
        {
            RunVoxelGeometry(m_cubeToWorld, 1, true);
            RestoreListState();
        }
        // The surfaces the face shows (its depth and colour) into the levels, outside the camera's view.
        if (!end)
            m_cubeFaceNoted = false;
        else if (m_cubeFaceNoted)
        {
            PassName("CryMT GI cube inject");
            RunCubeInject();
            PassNameEnd();
            RestoreListState();
        }
        return;
    }
    if (!m_passNamesOn)
        return;
    std::wstring name(text, text + strlen(text)); // ASCII
    m_list->BeginEvent(0, name.c_str(), static_cast<UINT>((name.size() + 1) * sizeof(wchar_t)));
    m_passNames.push_back(std::move(name));
    PassStamp(PassLabel());
}

void Device::PassNameEnd()
{
    if (!m_passNamesOn || m_passNames.empty())
        return;
    m_passLastEnded = std::string(m_passNames.back().begin(), m_passNames.back().end());
    m_passNames.pop_back();
    m_list->EndEvent();
    PassStamp(PassLabel());
}

void Device::Submit()
{
    std::lock_guard<std::recursive_mutex> guard(g.lock);
    SplitQueries(true);
    FlushBarriers();
    if (m_listStamps)
    {
        const UINT first = static_cast<UINT>(g_mainSerial % kListStamps) * 2;
        m_list->EndQuery(m_listStampHeap, D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        m_list->ResolveQueryData(m_listStampHeap, D3D12_QUERY_TYPE_TIMESTAMP, first, 2, m_listStampReadback, first * sizeof(uint64_t));
    }
    PassStampsSubmit();
    for (size_t i = 0; i < m_passNames.size(); i++)
        m_list->EndEvent();
    const HRESULT closed = m_list->Close();
    if (FAILED(closed))
        LogLimited("close", "main list Close failed %08lx", static_cast<unsigned long>(closed));
    ExecuteUploads();
    ID3D12CommandList* lists[] = {m_list};
    g.queue->ExecuteCommandLists(1, lists);
    const uint64_t fence = SignalQueue();
    if (!m_removedLogged && g.dev->GetDeviceRemovedReason() != S_OK)
    {
        Log("device removed: %08lx (after main list %llu)", static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()),
            static_cast<unsigned long long>(g_mainSerial));
        m_removedLogged = true;
        LogDred();
    }
    g_inFlight.push_back({g_mainSerial, fence});
    m_allocs.Put(m_alloc, fence);
    m_frameRing.Mark(fence);
    m_srvRing.Mark(fence);
    g_mainSerial++;
    Retire();
    if (m_listStamps)
    {
        const uint64_t done = g_doneSerial.load(std::memory_order_acquire);
        if (done > m_stampedSerial + kListStamps)
            m_stampedSerial = done - kListStamps; // overwritten before being read
        for (; m_stampedSerial < done; m_stampedSerial++)
        {
            const uint64_t* s = m_listStamps + ((m_stampedSerial + 1) % kListStamps) * 2;
            if (s[1] > s[0])
            {
                m_stats.gpuTicks += s[1] - s[0];
                m_stats.gpuLists++;
            }
        }
    }
    PassStampsRead();
    RunDeferred();
    m_frameRing.Reclaim();
    m_srvRing.Reclaim();
    g_initRing.Reclaim();
    BeginList();
    SplitQueries(false);
    m_stats.submissions++;
    DrainDebugMessages();
}

void Device::WaitIdle()
{
    Submit();
    WaitFence(g.signaled);
    std::lock_guard<std::recursive_mutex> guard(g.lock);
    Retire();
    RunDeferred();
}

void Device::ClearState()
{
    for (StageState& st : m_stage)
    {
        for (auto& cb : st.cb)
            Bind(cb, static_cast<Buffer12*>(nullptr));
        for (UINT i = 0; i < st.srvEnd; i++)
            Bind(st.srv[i], static_cast<Srv12*>(nullptr));
        for (auto& smp : st.sampler)
            Bind(smp, static_cast<Sampler12*>(nullptr));
        st.srvDirty = st.samplerDirty = true;
    }
    Bind(m_vs, static_cast<VS12*>(nullptr));
    Bind(m_gs, static_cast<GS12*>(nullptr));
    Bind(m_ps, static_cast<PS12*>(nullptr));
    Bind(m_layout, static_cast<Layout12*>(nullptr));
    for (VertexBinding& vb : m_vb)
        Bind(vb.buffer, static_cast<Buffer12*>(nullptr));
    Bind(m_ib, static_cast<Buffer12*>(nullptr));
    OMSetRenderTargets(0, nullptr, nullptr);
    Bind(m_blend, static_cast<Blend12*>(nullptr));
    Bind(m_depth, static_cast<Depth12*>(nullptr));
    Bind(m_raster, static_cast<Raster12*>(nullptr));
    m_topology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
    m_sampleMask = 0xFFFFFFFF;
    for (float& f : m_blendFactor)
        f = 1.0f;
    m_stencilRef = 0;
    m_numViewports = m_numScissors = 0;
    m_psoDirty = m_topologyDirty = m_viewportDirty = m_scissorDirty = m_blendFactorDirty = m_stencilRefDirty = true;
}

ID3D12PipelineState* Device::Pso(bool indexed)
{
    PsoKey k;
    memset(&k, 0, sizeof(k));
    k.vs = m_vs->Id();
    k.gs = m_gs ? m_gs->Id() : 0;
    k.ps = m_ps ? m_ps->Id() : 0;
    k.layout = m_layout ? m_layout->Id() : 0;
    k.blend = m_blend ? m_blend->Id() : 0;
    k.depth = m_depth ? m_depth->Id() : 0;
    const int cubeCull = m_engineCubePass ? EngineCubeCullFor(m_engineCubeFace) : 0;
    k.raster = (m_raster ? m_raster->Id() : 0) | static_cast<uint32_t>(cubeCull) << 28;
    k.sampleMask = m_sampleMask;
    UINT numRtv = 0;
    for (UINT i = 0; i < 8; i++)
        if (m_rtv[i])
        {
            numRtv = i + 1;
            k.rtv[i] = m_rtv[i]->format;
        }
    k.numRtv = static_cast<uint8_t>(numRtv);
    k.dsv = m_dsv ? m_dsv->format : DXGI_FORMAT_UNKNOWN;
    UINT samples = 1, quality = 0;
    if (numRtv && m_rtv[0])
        samples = m_rtv[0]->samples, quality = m_rtv[0]->quality;
    else if (m_dsv)
        samples = m_dsv->samples, quality = m_dsv->quality;
    k.samples = static_cast<uint8_t>(samples);
    k.quality = static_cast<uint8_t>(quality);
    k.topologyType = static_cast<uint8_t>(TopologyType(m_topology));
    k.stripCut = StripTopology(m_topology) ? (m_ibFormat == DXGI_FORMAT_R32_UINT ? 2 : 1) : 0;

    const auto found = m_psos.find(k);
    if (found != m_psos.end())
        return found->second;

    // The description by content: the name pipelines are recorded and created ahead under.
    PsoRecipe r;
    memset(&r, 0, sizeof(r));
    r.vs = m_vs->contentHash;
    r.gs = m_gs ? m_gs->contentHash : 0;
    r.ps = m_ps ? m_ps->contentHash : 0;
    r.layout = m_layout ? m_layout->contentHash : 0;
    SetBlend(r.blend, m_blend ? m_blend->d12 : Blend12From(kDefaultBlend));
    if (numRtv >= 2 && k.rtv[0] == DXGI_FORMAT_R16G16B16A16_FLOAT && k.rtv[1] == DXGI_FORMAT_R16_FLOAT)
    {
        // DLSS's bias mask (BindBiasMask) beside the scene's colour: blended as the colour, red only.
        r.blend.IndependentBlendEnable = TRUE;
        r.blend.RenderTarget[1] = r.blend.RenderTarget[0];
        r.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
        if (numRtv >= 3 && k.rtv[2] == DXGI_FORMAT_R16_FLOAT)
        {
            // The particle layer (BindParticleLayer): the nearest depth kept.
            D3D12_RENDER_TARGET_BLEND_DESC& p = r.blend.RenderTarget[2];
            p = {};
            p.BlendEnable = TRUE;
            p.SrcBlend = p.DestBlend = p.SrcBlendAlpha = p.DestBlendAlpha = D3D12_BLEND_ONE;
            p.BlendOp = p.BlendOpAlpha = D3D12_BLEND_OP_MIN;
            p.LogicOp = D3D12_LOGIC_OP_NOOP;
            p.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
        }
    }
    else if (numRtv >= 2 && k.rtv[0] == DXGI_FORMAT_R16G16B16A16_FLOAT && k.rtv[1] == DXGI_FORMAT_R16G16B16A16_FLOAT && m_vs->motionBlur)
    {
        // Exact object motion (BindObjectMotion) beside the motion blur target: written as it is.
        r.blend.IndependentBlendEnable = TRUE;
        r.blend.RenderTarget[1] = r.blend.RenderTarget[0];
        r.blend.RenderTarget[1].BlendEnable = FALSE;
        r.blend.RenderTarget[1].LogicOpEnable = FALSE;
        r.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    r.sampleMask = m_sampleMask;
    r.raster = m_raster ? m_raster->d12 : Raster12From(kDefaultRaster); // no padding
    if (cubeCull == 1)
        r.raster.CullMode = D3D12_CULL_MODE_NONE;
    else if (cubeCull == 2 && r.raster.CullMode != D3D12_CULL_MODE_NONE)
        r.raster.CullMode = r.raster.CullMode == D3D12_CULL_MODE_BACK ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
    SetDepth(r.depth, m_depth ? m_depth->d12 : Depth12From(kDefaultDepth));
    if (!m_dsv)
        r.depth.DepthEnable = r.depth.StencilEnable = FALSE;
    r.stripCut = k.stripCut == 2   ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                 : k.stripCut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                   : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    r.topologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(k.topologyType);
    r.numRtv = numRtv;
    for (UINT i = 0; i < numRtv; i++)
        r.rtv[i] = k.rtv[i];
    r.dsv = k.dsv;
    r.samples = {samples, quality};
    const PsoName name = NameOf(r);
    if (g_asyncPipelines && indexed && r.depth.DepthEnable)
    {
        const std::vector<uint8_t>* code[3] = {&m_vs->code, m_gs ? &m_gs->code : nullptr, m_ps ? &m_ps->code : nullptr};
        bool pending = false;
        ID3D12PipelineState* ready = TakePipelineAsync(name, r, code, m_layout ? &m_layout->elements : nullptr, pending);
        if (pending)
        {
            m_psoPending = true;
            return nullptr;
        }
        if (ready)
            m_stats.psoCreated++;
        m_psos.emplace(k, ready);
        return ready;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    ID3D12PipelineState* pso = TakePipeline(name);
    HRESULT hr = S_OK;
    if (!pso)
    {
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC d = DescOf(r, &m_vs->code, m_gs ? &m_gs->code : nullptr, m_ps ? &m_ps->code : nullptr,
                                                            m_layout ? &m_layout->elements : nullptr);
        hr = g.dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
        if (FAILED(hr))
            pso = nullptr;
        const std::vector<uint8_t>* code[3] = {&m_vs->code, m_gs ? &m_gs->code : nullptr, m_ps ? &m_ps->code : nullptr};
        PublishPipeline(name, r, pso, code, m_layout ? &m_layout->elements : nullptr);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    m_stats.psoMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
    // Pipelines created here (not ahead by the workers) stall the stream thread: when, for matching with slow frames.
    if ((t1.QuadPart - t0.QuadPart) * 1000 > freq.QuadPart / 10)
        Log("pipeline at draw: qpc %lld, %.2f ms", static_cast<long long>(t0.QuadPart), (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
    if (FAILED(hr))
    {
        m_stats.psoFailed++;
        char key[48];
        snprintf(key, sizeof(key), "pso%lx", static_cast<unsigned long>(hr));
        LogLimited(key, "CreateGraphicsPipelineState failed %08lx: vs %u gs %u ps %u layout %u rtv %u fmt0 %d dsv %d topo %d", static_cast<unsigned long>(hr), k.vs,
                   k.gs, k.ps, k.layout, numRtv, k.rtv[0], k.dsv, k.topologyType);
        pso = nullptr;
    }
    else
        m_stats.psoCreated++;
    m_psos.emplace(k, pso);
    return pso;
}

// A submission while applying (a ring ran out) starts a new list with nothing set, so applying starts over.
bool Device::Apply(bool indexed)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        const uint64_t serial = g_mainSerial;
        if (!ApplyOnce(indexed))
            return false;
        if (serial == g_mainSerial)
            return true;
    }
    return true;
}

bool Device::ApplyOnce(bool indexed)
{
    // The first draw of a frame that reads the GI placeholder (the general pass, after the z-prepass) runs the GI pass.
    // Only inside the scene: a pass after the scene was handed over (SceneDone) that reads it would otherwise run it with
    // its own depth and the old camera, and the next frame's scene would get that GI, a frame late.
    // Also only with the scene's depth buffer (the one bound when the last scene was handed over): passes before the
    // scene that read it too (the water reflection renders the scene from a mirrored camera, not every frame) would
    // run it with their own depth, and the scene would get that GI (zero or garbage: GI flickering from frame to frame).
    if (Gi().on && GiSolidFill() && m_giHist[0] && m_giLastRun != m_giFrame && m_ps && PsUsesGiPlaceholder())
    {
        if (!m_giInScene)
            m_stats.giLateReads++;
        else if (m_giSceneDepth && (!m_dsv || m_dsv->info != m_giSceneDepth || !m_rtv[0] || !SceneColourLike(m_rtv[0]->info)))
            m_stats.giOtherDepth++;
        else
        {
            PassName("CryMT GI");
            RunGi();
            PassNameEnd();
        }
    }
    // Bend's screen-space shadows: made once a frame, before the first draw reading them after the frame's z-prepass
    // (the deferred shadow pass). fragLib's materials read them too, some before the z-prepass (they get none): the
    // first such draw took the frame's turn and found no depth (0 frames made).
    // The water's depth pyramid: once a frame, before the first draw reading it (the ocean), after the z-prepass.
    if (m_ps && g_hizPlaceholder && m_hizTriedFrame != m_frame && m_zTargetFrame == m_frame && PsReads(g_hizPlaceholder))
    {
        m_hizTriedFrame = m_frame;
        if (WaterSsrOn())
        {
            PassName("CryMT HiZ");
            RunHiz();
            PassNameEnd();
        }
    }
    if (m_ps && g_sssPlaceholder && m_sssTriedFrame != m_frame && m_zTargetFrame == m_frame && PsReads(g_sssPlaceholder))
    {
        m_sssTriedFrame = m_frame;
        if (Gi().contactShadows && Gi().sss && m_sssSun)
        {
            PassName("CryMT SSS");
            RunSss();
            PassNameEnd();
        }
    }
    // The GI in the fog: once a frame, before the scene's fog pass (the patched FogPass: its pixel shader declares
    // CryMT_VFogSampler). The engine's post-process code binds the pass's textures itself: what it leaves in that slot
    // is logged (the first few times), as the pass without CryMT's back end reads it.
    if (m_ps && m_ps->vfogSlot >= 0 && m_vfogTriedFrame != m_frame)
    {
        static int logged = 0;
        if (logged < 6)
        {
            logged++;
            const Srv12* v = m_ps->vfogSlot < static_cast<int>(kTableSrvs) ? m_stage[kPS].srv[m_ps->vfogSlot] : nullptr;
            const auto* t = v && v->info && !v->info->buffer ? static_cast<const TextureCore*>(v->info) : nullptr;
            Log("volumetric fog: fog pass draw, slot t%d holds %s %ux%ux%u dim %d fmt %d; target %ux%u (scene %ux%u)", m_ps->vfogSlot,
                !v ? "nothing" : v->info == g_vfogPlaceholder ? "the placeholder" : "another texture", t ? t->width : 0, t ? t->height : 0,
                t ? t->depthOrArray : 0, t ? static_cast<int>(t->dim) : -1, t ? static_cast<int>(t->format) : -1,
                m_rtv[0] && m_rtv[0]->info && !m_rtv[0]->info->buffer ? static_cast<const TextureCore*>(m_rtv[0]->info)->width : 0,
                m_rtv[0] && m_rtv[0]->info && !m_rtv[0]->info->buffer ? static_cast<const TextureCore*>(m_rtv[0]->info)->height : 0, m_giFullW, m_giFullH);
        }
        PassName("CryMT VFog");
        RunVfog();
        PassNameEnd();
    }
    if (!m_vs || m_topology == D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED)
    {
        m_stats.skippedDraws++;
        return false;
    }
    if (m_ps && m_rtv[0] && m_rtv[0]->format == DXGI_FORMAT_R32_FLOAT && m_dsv)
    {
        m_stats.zpassDraws++;
        if (m_ps->writesTarget1)
            m_stats.zpassTarget1++;
    }
    if (m_ps && m_ps->writesTarget1 && m_rtv[0] && !m_rtv[1] && m_rtv[0]->format == DXGI_FORMAT_R32_FLOAT && m_rtv[0]->samples == 1)
        BindFoliageMask();
    // The patched object motion blur pass (exact object motion in SV_Target1) into its own target: its colour target is
    // RGBA16F of the scene's size too, so before the bias mask's test.
    if (m_ps && m_ps->writesTarget1 && m_vs && m_vs->motionBlur && m_rtv[0] && !m_rtv[1] && m_rtv[0]->format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        m_rtv[0]->samples == 1)
        BindObjectMotion();
    // Water and particles (the patched pixel shaders write DLSS's bias mask in SV_Target1) into the scene's colour.
    if (m_ps && m_ps->writesTarget1 && (!m_ps->writesTarget2 || m_ps->particleLayer) && !(m_vs && m_vs->motionBlur) && m_rtv[0] && !m_rtv[1] &&
        m_rtv[0]->format == DXGI_FORMAT_R16G16B16A16_FLOAT && m_rtv[0]->samples == 1 && m_foliageW &&
        static_cast<const TextureCore*>(m_rtv[0]->info)->width == m_foliageW && static_cast<const TextureCore*>(m_rtv[0]->info)->height == m_foliageH)
        BindBiasMask();
    // The particles' layer for the water's SSR (SV_Target2) beside the bias mask.
    if (m_ps && m_ps->particleLayer && m_biasRtv && m_rtv[1] == m_biasRtv && !m_rtv[2])
        BindParticleLayer();
    if (m_pastScene && m_rtv[0] && m_rtv[0]->info == m_doneColour)
    {
        m_note.lateDraws++;
        if (m_dsv)
            m_note.late3D++;
    }
    if (m_psoDirty)
    {
        m_psoPending = false;
        ID3D12PipelineState* pso = Pso(indexed);
        m_psoValid = pso != nullptr;
        if (pso && pso != m_psoApplied)
        {
            m_list->SetPipelineState(pso);
            m_psoApplied = pso;
            m_stats.psoSwitches++;
        }
        m_psoDirty = m_psoPending; // asked again at the next draw while its pipeline is being created
    }
    if (!m_psoValid)
    {
        m_stats.skippedDraws++;
        return false;
    }
    if (m_topologyDirty)
    {
        m_list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(m_topology));
        m_topologyDirty = false;
    }

    // Output: states every draw (copies and clears move them), bindings when changed.
    UINT numRtv = 0;
    for (UINT i = 0; i < 8; i++)
        if (m_rtv[i])
        {
            numRtv = i + 1;
            Require(m_rtv[i]->info, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    if (m_dsv)
        Require(m_dsv->info, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (m_omDirty)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv[8];
        for (UINT i = 0; i < numRtv; i++)
            rtv[i] = m_rtv[i] ? m_rtv[i]->cpu : m_nullRtv;
        m_list->OMSetRenderTargets(numRtv, numRtv ? rtv : nullptr, FALSE, m_dsv ? &m_dsv->cpu : nullptr);
        m_omDirty = false;
    }

    // Vertex and index buffers: views compared with the applied ones (dynamic buffers rename under the same binding).
    const UINT vbEnd = m_layout ? m_layout->slotEnd : 0;
    UINT first = 16, last = 0;
    for (UINT i = 0; i < vbEnd; i++)
    {
        D3D12_VERTEX_BUFFER_VIEW v{};
        if (Buffer12* b = m_vb[i].buffer)
        {
            v.BufferLocation = b->Gpu() + m_vb[i].offset;
            v.SizeInBytes = b->desc.ByteWidth > m_vb[i].offset ? b->desc.ByteWidth - m_vb[i].offset : 0;
            v.StrideInBytes = m_vb[i].stride;
            MarkRead(b);
        }
        if (memcmp(&v, &m_vbApplied[i], sizeof(v)) != 0)
        {
            m_vbApplied[i] = v;
            first = std::min(first, i);
            last = i + 1;
        }
    }
    if (first < last)
        m_list->IASetVertexBuffers(first, last - first, &m_vbApplied[first]);
    if (indexed)
    {
        D3D12_INDEX_BUFFER_VIEW v{};
        if (m_ib)
        {
            v.BufferLocation = m_ib->Gpu() + m_ibOffset;
            v.SizeInBytes = m_ib->desc.ByteWidth > m_ibOffset ? m_ib->desc.ByteWidth - m_ibOffset : 0;
            v.Format = m_ibFormat;
            MarkRead(m_ib);
        }
        if (memcmp(&v, &m_ibApplied, sizeof(v)) != 0)
        {
            m_ibApplied = v;
            m_list->IASetIndexBuffer(&v);
        }
    }

    ApplyStage(kVS, m_vs->cbMask, m_vs->srvEnd, m_vs->samplerEnd);
    if (m_gs)
        ApplyStage(kGS, m_gs->cbMask, m_gs->srvEnd, m_gs->samplerEnd);
    if (m_ps)
        ApplyStage(kPS, m_ps->cbMask, m_ps->srvEnd, m_ps->samplerEnd);

    if (m_viewportDirty && m_numViewports)
    {
        m_list->RSSetViewports(m_numViewports, m_viewports);
        m_viewportDirty = false;
    }
    if (m_scissorDirty)
    {
        D3D12_RECT rects[16];
        const bool scissor = m_raster && m_raster->desc.ScissorEnable;
        const UINT n = std::max(1u, m_numViewports);
        for (UINT i = 0; i < n; i++)
            rects[i] = scissor && i < m_numScissors ? m_scissors[i] : D3D12_RECT{0, 0, D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX};
        m_list->RSSetScissorRects(n, rects);
        m_scissorDirty = false;
    }
    if (m_blendFactorDirty)
    {
        m_list->OMSetBlendFactor(m_blendFactor);
        m_blendFactorDirty = false;
    }
    if (m_stencilRefDirty)
    {
        m_list->OMSetStencilRef(m_stencilRef);
        m_stencilRefDirty = false;
    }
    FlushBarriers();
    return true;
}

void Device::ApplyStage(Stage s, uint32_t cbMask, UINT srvEnd, UINT samplerEnd)
{
    StageState& st = m_stage[s];
    for (uint32_t mask = cbMask; mask;)
    {
        const UINT i = static_cast<UINT>(__builtin_ctz(mask));
        mask &= mask - 1;
        if (i >= kRootCbSlots[s])
        {
            LogLimited("cbslot", "stage %d uses constant buffer slot %u beyond the root signature", s, i);
            continue;
        }
        Buffer12* b = st.cb[i];
        const D3D12_GPU_VIRTUAL_ADDRESS va = b ? b->Gpu() : m_zeroCb->GetGPUVirtualAddress();
        if (b)
            MarkRead(b);
        if (va != st.cbApplied[i])
        {
            m_list->SetGraphicsRootConstantBufferView(kRootCb[s] + i, va);
            st.cbApplied[i] = va;
        }
    }

    if (srvEnd > kTableSrvs)
    {
        LogLimited("srvslot", "stage %d uses texture slot %u beyond the root signature", s, srvEnd - 1);
        srvEnd = kTableSrvs;
    }
    for (UINT i = 0; i < srvEnd; i++)
        if (Srv12* v = st.srv[i])
        {
            if (v->info->buffer)
            {
                auto* b = static_cast<Buffer12*>(v->info);
                MarkRead(b);
                if (b->Dynamic() && v->generation != b->generation)
                    st.srvDirty = true;
            }
            else
            {
                Require(v->info, kSrvState);
                if (m_pastScene && v->info == m_doneColour)
                    m_note.colourReads++;
                if (m_pastScene && v->info != m_doneColour)
                    for (uint32_t c = 0; c < m_preCopyCount; c++)
                        if (m_preCopies[c] == v->info)
                        {
                            m_note.copyReads++;
                            if (CopyLog() && m_preCopyReads[c].size() < 600)
                            {
                                char text[200];
                                snprintf(text, sizeof(text), "\n    read by [%s] stage %d slot %u ps %016llx", CurrentPass().c_str(), static_cast<int>(s), i,
                                         static_cast<unsigned long long>(m_ps ? m_ps->contentHash : 0));
                                m_preCopyReads[c] += text;
                            }
                        }
                // Before the hand-over, a full-size draw that only copies a scene-sized colour into another one (a plain
                // copy shader): a copy of the jittered scene. Other passes reading the scene (the water into the scene
                // itself) are not copies: DLSS's picture must not replace what they made.
                if (!m_pastScene && s == kPS && m_rtv[0] && m_rtv[0]->info != v->info && LikeSceneColour(v->info) &&
                    LikeSceneColour(m_rtv[0]->info) && m_ps && m_ps->plainCopy)
                    NotePreCopy(m_rtv[0]->info, "copy shader");
                if (s == kPS && m_foliageSrv && v->info == g_foliagePlaceholder)
                    Require(m_foliageSrv->info, kSrvState);
                if (s == kPS && v->info == g_sssPlaceholder && m_sssFrame == m_frame)
                    Require(&m_sss, kSrvState);
                if (s == kPS && v->info == g_hizPlaceholder && m_hizFrame == m_frame)
                    Require(&m_hiz, kSrvState);
                if (s == kPS && v->info == g_ssrPartPlaceholder && m_ssrPrevOn && LastParticleLayer())
                    Require(LastParticleLayer()->info, kSrvState);
                // The deferred shadow pass (patched ShadowMaskGen: it reads the frame texture) draws into the screen's
                // shadow mask (the sun in red, as the shaders read it): kept for GTAO.
                if (s == kPS && v->info == g_jitterPlaceholder && m_rtv[0] && m_rtv[0]->samples == 1 && m_rtv[0] != m_shadowMaskRtv)
                {
                    Bind(m_shadowMaskRtv, m_rtv[0]);
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        const auto* t = static_cast<const TextureCore*>(m_rtv[0]->info);
                        Log("sun shadow mask: %ux%u format %d", t->width, t->height, static_cast<int>(m_rtv[0]->format));
                    }
                }
            }
        }
    if (s == kPS && m_giSrv)
        Require(m_giSrv->info, kSrvState);
    EnvView envSub[2] = {};
    // fragLib's reflection on every material but water, glass, eyes, hair and skin (the black placeholder there).
    EnvView envAll;
    if (s == kPS && (!g_giControl.active || g_giControl.envAll) && m_ps && !m_ps->refractive && !m_ps->noEnv)
    {
        if (EnvEngineOn() && m_ldrCubeReady)
        {
            envAll.info = &m_ldrCube;
            envAll.cpu = m_ldrCubeSrv;
        }
        else if (EnvOn() && m_envReady)
        {
            envAll.info = m_envCubeAll->info;
            envAll.cpu = m_envCubeAll->cpu;
        }
    }
    if (envAll.info && st.srvDirty)
        Require(envAll.info, kSrvState);
    // The ocean's reflection cube (Water.cfx reads it in place of its placeholder; water is "refractive", so not envAll).
    const EnvView waterEnv = s == kPS ? WaterEnvCube() : EnvView{};
    if (waterEnv.info && st.srvDirty)
        Require(waterEnv.info, kSrvState);
    if (s == kPS && m_ps && st.srvDirty)
        for (int k = 0; k < 2; k++)
            if (const int slot = k ? m_ps->env2DSlot : m_ps->envCubeSlot; slot >= 0 && slot < static_cast<int>(kTableSrvs) && st.srv[slot])
                if ((envSub[k] = EnvSubstitute(slot)).info)
                {
                    Require(envSub[k].info, kSrvState);
                    m_envBinds++;
                    if (k == 1)
                        m_env2DBinds++;
                }
    // The table always holds all bound views (and the GI slot), so a later shader reading further slots finds them.
    if (srvEnd && st.srvDirty)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        while (!m_srvRing.TryAlloc(kTableSrvs, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
        D3D12_CPU_DESCRIPTOR_HANDLE src[kTableSrvs];
        for (UINT i = 0; i < kTableSrvs; i++)
        {
            Srv12* v = st.srv[i];
            if (v)
                v->Refresh();
            src[i] = v ? v->cpu : g_nullSrv;
            if (v && s == kPS && m_giCpu.ptr && v->info == g_giPlaceholder && (m_giSrv || m_giRunFrame + 2 >= m_frame))
                src[i] = m_giCpu; // not a pass of long ago (frames without a main camera: the binoculars' frozen GTAO)
            if (v && s == kPS && m_jitterCpu.ptr && v->info == g_jitterPlaceholder)
                src[i] = m_jitterCpu;
            if (v && s == kPS && m_fplusCpu.ptr && v->info == g_fplusPlaceholder)
                src[i] = m_fplusCpu;
            if (v && envSub[0].info && static_cast<int>(i) == m_ps->envCubeSlot)
                src[i] = envSub[0].cpu;
            if (v && envAll.info && v->info == g_envPlaceholder)
                src[i] = envAll.cpu;
            if (v && waterEnv.info && v->info == g_waterEnvPlaceholder)
                src[i] = waterEnv.cpu;
            if (v && envSub[1].info && static_cast<int>(i) == m_ps->env2DSlot)
                src[i] = envSub[1].cpu;
            if (v && s == kPS && m_foliageSrv && v->info == g_foliagePlaceholder)
                src[i] = m_foliageSrv->cpu;
            if (v && s == kPS && v->info == g_sssPlaceholder && m_sssFrame == m_frame)
                src[i] = m_sssSrv;
            if (v && s == kPS && v->info == g_hizPlaceholder && m_hizFrame == m_frame)
                src[i] = m_hizSrv;
            if (v && s == kPS && v->info == g_ssrPartPlaceholder && m_ssrPrevOn && LastParticleLayer())
                src[i] = LastParticleLayer()->cpu;
            if (v && s == kPS && v->info == g_ssrPrevPlaceholder && m_ssrPrevOn && m_ssrPrevSrv.ptr)
            {
                src[i] = m_ssrPrevSrv;
                if (m_ssrPrevBinds++ == 0)
                    Log("water SSR: the previous frame's scene bound (frame %llu)", static_cast<unsigned long long>(m_frame));
            }
            // The GI in the fog: the patched fog pass's two slots always filled (the engine's post-process code binds only
            // its own textures; whatever another pass left in these stays: the editor, 2026-11-15): this frame's volume
            // and parameters for the scene's fog pass, else typed null views (no mark: the pass keeps the engine's fog).
            if (s == kPS && m_ps && m_ps->vfogSlot >= 0 && (static_cast<int>(i) == m_ps->vfogSlot || static_cast<int>(i) == m_ps->vfogParamSlot))
            {
                const bool valid = m_vfogFrame == m_frame && m_rtv[0] && m_rtv[0]->info == m_vfogTarget && m_vfogParamSrv.ptr;
                if (static_cast<int>(i) == m_ps->vfogSlot)
                    src[i] = valid ? m_vfogSrv : VfogNull3d();
                else
                    src[i] = valid ? m_vfogParamSrv : g_nullSrv;
            }
            if (v && s == kPS && m_pvolBuilt && m_pvolCpu.ptr && v->info == g_pvolPlaceholder && Gi().on && Gi().particlesGi)
                src[i] = m_pvolCpu;
        }
        const UINT count = kTableSrvs;
        g.dev->CopyDescriptors(1, &cpu, &count, kTableSrvs, src, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_list->SetGraphicsRootDescriptorTable(kRootSrv[s], gpu);
        st.srvTable = gpu;
        st.srvApplied = kTableSrvs;
        st.srvDirty = false;
        m_stats.srvTables++;
    }

    samplerEnd = std::min(samplerEnd, kTableSamplers);
    // The water reading CryMT's cube: its sampler is a trilinear clamp of ours (the engine's state for the placeholder was
    // point: the reflection came out in steps on calm water, and the mips need a linear blend).
    const int waterSampler = waterEnv.info && m_ps && m_ps->waterEnvSamplerSlot >= 0 && m_ps->waterEnvSamplerSlot < static_cast<int>(samplerEnd)
                                 ? m_ps->waterEnvSamplerSlot : -1;
    if (waterSampler >= 0)
    {
        st.samplerDirty = true;
        if (!m_trilinearClamp.ptr)
        {
            m_trilinearClamp = g_samplerCpu.Alloc();
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            g.dev->CreateSampler(&sd, m_trilinearClamp);
        }
    }
    if (samplerEnd && (st.samplerDirty || samplerEnd > st.samplerApplied))
    {
        SamplerKey key;
        memset(&key, 0, sizeof(key));
        key.count = samplerEnd;
        for (UINT i = 0; i < samplerEnd; i++)
            key.ids[i] = st.sampler[i] ? st.sampler[i]->Id() : 0;
        if (waterSampler >= 0)
            key.ids[waterSampler] = 0xFFFFFFF0u; // m_trilinearClamp
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        const auto found = m_samplerTables.find(key);
        if (found != m_samplerTables.end())
            gpu = found->second;
        else
        {
            // Tables hold just the samplers the shader reads; the heap holds 2048, so tables are cached for good.
            D3D12_CPU_DESCRIPTOR_HANDLE cpu;
            if (!m_samplerHeap.TryAlloc(samplerEnd, cpu, gpu))
            {
                // Full: start over once the GPU no longer reads any table.
                WaitIdle();
                m_samplerHeap.head = m_samplerHeap.tail = 0;
                m_samplerTables.clear();
                m_stats.samplerResets++;
                m_samplerHeap.TryAlloc(samplerEnd, cpu, gpu);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE src[kTableSamplers];
            for (UINT i = 0; i < samplerEnd; i++)
                src[i] = st.sampler[i] ? st.sampler[i]->cpu : g_defaultSampler;
            if (waterSampler >= 0)
                src[waterSampler] = m_trilinearClamp;
            const UINT count = samplerEnd;
            g.dev->CopyDescriptors(1, &cpu, &count, samplerEnd, src, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            m_samplerTables.emplace(key, gpu);
            m_stats.samplerTables++;
        }
        m_list->SetGraphicsRootDescriptorTable(kRootSampler[s], gpu);
        st.samplerApplied = samplerEnd;
        st.samplerDirty = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Updates and dynamic textures.
// ---------------------------------------------------------------------------------------------------------------------
void Device::UpdateSubresource(ID3D10Resource* dst, UINT sub, const D3D10_BOX* box, const void* data, UINT rowPitch, UINT depthPitch)
{
    if (!dst || !data)
        return;
    if (Buffer12* b = AsBuffer(dst))
    {
        if (!b->Page())
        {
            LogLimited("updbuf", "UpdateSubresource on a dynamic or staging buffer is not supported");
            return;
        }
        const UINT from = box ? box->left : 0;
        const UINT bytes = box ? box->right - box->left : b->desc.ByteWidth;
        if (!bytes)
            return;
        const UploadAlloc up = FrameUpload(bytes, 16);
        if (!up.cpu)
            return;
        memcpy(up.cpu, data, bytes);
        if (b->desc.BindFlags & D3D10_BIND_CONSTANT_BUFFER)
        {
            if (from == 0 && bytes >= sizeof(b->head))
            {
                memcpy(b->head, data, sizeof(b->head));
                b->headKnown = true;
            }
            else if (from < sizeof(b->head))
                b->headKnown = false;
        }
        PrepareCopyDest(b->Page());
        FlushBarriers();
        m_list->CopyBufferRegion(b->Res(), b->Offset() + from, up.res, up.offset, bytes);
        return;
    }
    TextureCore* t = AsTexture(dst);
    if (!t || t->Staging() || sub >= t->subresources)
        return;
    const UINT mip = sub % t->mips;
    UINT w = std::max(1u, t->width >> mip);
    UINT h = t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D ? 1 : std::max(1u, t->height >> mip);
    UINT d = t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? std::max(1u, t->depthOrArray >> mip) : 1;
    UINT x = 0, y = 0, z = 0;
    if (box)
    {
        x = box->left;
        y = box->top;
        z = box->front;
        w = box->right - box->left;
        h = box->bottom - box->top;
        d = box->back - box->front;
    }
    if (!w || !h || !d)
        return;
    const bool bc = BlockCompressed(t->format);
    const UINT rows = bc ? (h + 3) / 4 : h;
    const UINT rowBytes = bc ? (w + 3) / 4 * BlockBytes(t->format) : w * FormatBytes(t->format);
    const UINT pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UploadAlloc up = FrameUpload(static_cast<uint64_t>(pitch) * rows * d, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!up.cpu)
        return;
    for (UINT zz = 0; zz < d; zz++)
        for (UINT r = 0; r < rows; r++)
            memcpy(up.cpu + (static_cast<uint64_t>(zz) * rows + r) * pitch,
                   static_cast<const uint8_t*>(data) + static_cast<uint64_t>(zz) * depthPitch + static_cast<uint64_t>(r) * rowPitch, rowBytes);
    Require(t, D3D12_RESOURCE_STATE_COPY_DEST);
    FlushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = t->res;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = up.res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = up.offset;
    sl.PlacedFootprint.Footprint = {t->format, bc ? (w + 3) & ~3u : w, bc ? (h + 3) & ~3u : h, d, pitch};
    NoteCopy("UpdateSubresource", dl, x, y, z, sl, nullptr);
    m_list->CopyTextureRegion(&dl, x, y, z, &sl, nullptr);
}

HRESULT TextureCore::MapSub(UINT sub, D3D10_MAP type, UINT flags, void** data, UINT* rowPitch, UINT* depthPitch)
{
    if (!data || sub >= subresources)
        return E_INVALIDARG;
    if (Staging())
    {
        const HRESULT hr = WaitStaging(this, type == D3D10_MAP_READ || type == D3D10_MAP_READ_WRITE, flags);
        if (FAILED(hr))
            return hr;
        *data = stagingCpu + footprints[sub].Offset;
        if (rowPitch)
            *rowPitch = footprints[sub].Footprint.RowPitch;
        if (depthPitch)
            *depthPitch = footprints[sub].Footprint.RowPitch * rows[sub];
        return S_OK;
    }
    if (usage != D3D10_USAGE_DYNAMIC || type != D3D10_MAP_WRITE_DISCARD)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC d = Desc12();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT n = 0;
    UINT64 rowSize = 0, total = 0;
    g.dev->GetCopyableFootprints(&d, sub, 1, 0, &fp, &n, &rowSize, &total);
    const UploadAlloc up = g_device->FrameUpload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!up.cpu)
        return E_OUTOFMEMORY;
    fp.Offset = up.offset;
    pending[sub] = {up, fp, true};
    *data = up.cpu;
    if (rowPitch)
        *rowPitch = fp.Footprint.RowPitch;
    if (depthPitch)
        *depthPitch = fp.Footprint.RowPitch * n;
    return S_OK;
}

void TextureCore::UnmapSub(UINT sub)
{
    if (Staging() || sub >= pending.size() || !pending[sub].active)
        return;
    pending[sub].active = false;
    g_device->UploadPending(this, sub, pending[sub]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Creation.
// ---------------------------------------------------------------------------------------------------------------------
HRESULT Device::CreateBuffer(const D3D10_BUFFER_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Buffer** out)
{
    if (!desc || !desc->ByteWidth)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* b = new Buffer12;
    b->desc = *desc;
    const void* initData = init ? init->pSysMem : nullptr;
    if (desc->Usage == D3D10_USAGE_DYNAMIC)
    {
        b->slice = g_slices.Alloc(desc->ByteWidth);
        b->prod = b->slice;
        g_buffer12Vtable = *reinterpret_cast<void* const*>(static_cast<ID3D10Buffer*>(b));
        if (!b->slice.cpu)
        {
            Log("CreateBuffer: no upload memory for a dynamic buffer of %u bytes (pages %llu), device removed reason %08lx", desc->ByteWidth,
                static_cast<unsigned long long>(g_slices.pages), static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
            b->Release();
            return E_OUTOFMEMORY;
        }
        if (initData)
            memcpy(b->slice.cpu, initData, desc->ByteWidth);
    }
    else if (desc->Usage == D3D10_USAGE_STAGING)
    {
        b->res = CreateStagingBuffer(desc->ByteWidth);
        if (!b->res)
        {
            b->Release();
            return E_OUTOFMEMORY;
        }
        void* p = nullptr;
        b->res->Map(0, nullptr, &p);
        b->stagingCpu = static_cast<uint8_t*>(p);
        if (initData)
            memcpy(b->stagingCpu, initData, desc->ByteWidth);
    }
    else
    {
        if (initData && (desc->BindFlags & D3D10_BIND_CONSTANT_BUFFER) && desc->ByteWidth >= sizeof(b->head))
        {
            memcpy(b->head, initData, sizeof(b->head));
            b->headKnown = true;
        }
        b->block = g_blocks.Alloc(desc->ByteWidth);
        if (!b->block.page)
        {
            Log("CreateBuffer: no default buffer memory for %u bytes (pages %llu), device removed reason %08lx", desc->ByteWidth,
                static_cast<unsigned long long>(g_blocks.pages), static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
            b->Release();
            return E_OUTOFMEMORY;
        }
        if (initData)
        {
            std::lock_guard<std::recursive_mutex> guard(g.lock);
            const UploadAlloc up = InitUpload(desc->ByteWidth, 16);
            if (up.cpu)
            {
                memcpy(up.cpu, initData, desc->ByteWidth);
                UploadList()->CopyBufferRegion(b->block.page->res, b->block.offset, up.res, up.offset, desc->ByteWidth);
            }
        }
    }
    *out = b;
    return S_OK;
}

void CopyInit(uint8_t* base, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, const UINT* rows, const UINT64* rowBytes,
              const D3D10_SUBRESOURCE_DATA* init, UINT n)
{
    for (UINT s = 0; s < n; s++)
    {
        const auto* src = static_cast<const uint8_t*>(init[s].pSysMem);
        if (!src)
            continue;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = layouts[s];
        for (UINT z = 0; z < fp.Footprint.Depth; z++)
            for (UINT r = 0; r < rows[s]; r++)
                memcpy(base + fp.Offset + (static_cast<uint64_t>(z) * rows[s] + r) * fp.Footprint.RowPitch,
                       src + static_cast<uint64_t>(z) * init[s].SysMemSlicePitch + static_cast<uint64_t>(r) * init[s].SysMemPitch, rowBytes[s]);
    }
}

HRESULT FinishTexture(TextureCore* t, const D3D10_SUBRESOURCE_DATA* init)
{
    const bool is3D = t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D;
    if (!t->mips)
    {
        UINT size = std::max(t->width, std::max(t->height, is3D ? t->depthOrArray : 1u));
        t->mips = 1;
        while (size > 1)
        {
            size >>= 1;
            t->mips++;
        }
    }
    t->subresources = t->mips * (is3D ? 1 : t->depthOrArray);
    const D3D12_RESOURCE_DESC d = t->Desc12();
    const UINT n = t->subresources;
    if (t->Staging())
    {
        t->footprints.resize(n);
        t->rows.resize(n);
        std::vector<UINT64> rowBytes(n);
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&d, 0, n, 0, t->footprints.data(), t->rows.data(), rowBytes.data(), &total);
        t->res = CreateStagingBuffer(total);
        if (!t->res)
            return E_OUTOFMEMORY;
        void* p = nullptr;
        t->res->Map(0, nullptr, &p);
        t->stagingCpu = static_cast<uint8_t*>(p);
        if (init)
            CopyInit(t->stagingCpu, t->footprints.data(), t->rows.data(), rowBytes.data(), init, n);
        return S_OK;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES state = (t->bind & D3D10_BIND_DEPTH_STENCIL)   ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                  : (t->bind & D3D10_BIND_RENDER_TARGET) ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                                         : D3D12_RESOURCE_STATE_COPY_DEST;
    if (init)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    // Optimized clear values: every depth clear the renderer makes is to 1, colour targets get 0. (Measured on this
    // NVIDIA driver: no GPU time difference either way; kept as the API asks.)
    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* clearValue = nullptr;
    if (t->bind & D3D10_BIND_DEPTH_STENCIL)
    {
        clear.Format = DepthViewFormat(t->format);
        clear.DepthStencil.Depth = 1.0f;
        clearValue = &clear;
    }
    else if ((t->bind & D3D10_BIND_RENDER_TARGET) && !TypelessColor(t->format))
    {
        clear.Format = t->format;
        clearValue = &clear;
    }
    const HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clearValue, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&t->res));
    if (FAILED(hr))
    {
        Log("CreateCommittedResource texture %ux%ux%u mips %u fmt %d bind 0x%x failed %08lx", t->width, t->height, t->depthOrArray, t->mips, t->format,
            t->bind, static_cast<unsigned long>(hr));
        return hr;
    }
    t->state = state;
    if (t->usage == D3D10_USAGE_DYNAMIC)
        t->pending.resize(n);
    if (init)
    {
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(n);
        std::vector<UINT> rows(n);
        std::vector<UINT64> rowBytes(n);
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&d, 0, n, 0, layouts.data(), rows.data(), rowBytes.data(), &total);
        std::lock_guard<std::recursive_mutex> guard(g.lock);
        const UploadAlloc up = InitUpload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!up.cpu)
            return E_OUTOFMEMORY;
        CopyInit(up.cpu, layouts.data(), rows.data(), rowBytes.data(), init, n);
        ID3D12GraphicsCommandList* list = UploadList();
        for (UINT s = 0; s < n; s++)
        {
            D3D12_TEXTURE_COPY_LOCATION dl{};
            dl.pResource = t->res;
            dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dl.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION sl{};
            sl.pResource = up.res;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sl.PlacedFootprint = layouts[s];
            sl.PlacedFootprint.Offset += up.offset;
            NoteCopy("initial data", dl, 0, 0, 0, sl, nullptr);
            list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        }
        const D3D12_RESOURCE_STATES after = (t->bind & D3D10_BIND_SHADER_RESOURCE)   ? kSrvState
                                            : (t->bind & D3D10_BIND_RENDER_TARGET) ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                                                   : D3D12_RESOURCE_STATE_COPY_DEST;
        if (after != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            const D3D12_RESOURCE_BARRIER b = Transition(t->res, D3D12_RESOURCE_STATE_COPY_DEST, after);
            list->ResourceBarrier(1, &b);
        }
        t->state = after;
    }
    return S_OK;
}

template <class T, class Desc>
void FillTexture(T* t, const Desc& desc, D3D10_RESOURCE_DIMENSION dim, UINT width, UINT height, UINT depthOrArray, UINT samples, UINT quality)
{
    t->desc = desc;
    t->dim = dim;
    t->usage = desc.Usage;
    t->bind = desc.BindFlags;
    t->cpuAccess = desc.CPUAccessFlags;
    t->misc = desc.MiscFlags;
    t->format = desc.Format;
    t->width = width;
    t->height = height;
    t->depthOrArray = depthOrArray;
    t->mips = desc.MipLevels;
    t->samples = samples;
    t->quality = quality;
}

HRESULT Device::CreateTexture1D(const D3D10_TEXTURE1D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture1D** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* t = new Texture1D12;
    FillTexture(t, *desc, D3D10_RESOURCE_DIMENSION_TEXTURE1D, desc->Width, 1, desc->ArraySize, 1, 0);
    const HRESULT hr = FinishTexture(t, init);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    t->desc.MipLevels = t->mips;
    *out = t;
    return S_OK;
}

// Textures/CryMT/gi.dds: CryMT's own 4x1 all-zero placeholder, the texture of the GI sampler in the patched fragLib.cfi
// and terrain.cfx (CryMT_GIAmbient: ambient * (1 - a) + rgb). The engine loads and binds it like any texture; pixel
// shaders see CryMT's GI texture in its place, and without CryMT's back end its zeros leave the ambient as it was.
// The materials' environment cube maps (envMapSamplerCUBE = $EnvironmentCubeMap in the game's shaders: all 1824
// Environment slots of the shipped .mtl files are files such as textures/cubemaps/beach_blur.dds): cube textures created
// with data and only read by shaders. Render-target cubes (the engine's own dynamic ones, light cubes) are not.
bool IsEnvCube(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    return (d.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE) && init && init->pSysMem && d.BindFlags == D3D10_BIND_SHADER_RESOURCE &&
           d.Usage != D3D10_USAGE_DYNAMIC && d.Width >= 8;
}

// Stage 0 of the dynamic environment maps, a test: CRYMT_ENVTEST=RRGGBB replaces every environment cube map with one
// cube of that colour, so which materials read them shows on screen.
uint32_t EnvTestColour()
{
    static int colour = -2;
    if (colour == -2)
    {
        char v[16] = {};
        colour = GetEnvironmentVariableA("CRYMT_ENVTEST", v, sizeof(v)) ? static_cast<int>(strtoul(v, nullptr, 16) & 0xFFFFFF) : -1;
    }
    return static_cast<uint32_t>(colour);
}

// Textures/CryMT/env.dds (CryMTSettings): a 4x4 cube, black, face 0's second texel E7000000 (the mark, alpha only).
bool IsEnvPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || !(d.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE) || d.Width != 4 || d.Height != 4 || d.ArraySize != 6)
        return false;
    const auto* p = static_cast<const uint32_t*>(init->pSysMem);
    return p[0] == 0 && p[1] == 0xE7000000u;
}

bool IsGiPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 4 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 16; i++)
        if (p[i] != 0)
            return false;
    return true;
}

// Textures/CryMT/jitter.dds: CryMT's 16x1 placeholder for the frame texture (patched ShadowMaskGen.cfx): texels 0..14
// zero (what the shader reads: no jitter, no contact shadows without CryMT), the last 5A5A5A5A (the mark).
bool IsJitterPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 16 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 64; i++)
        if (p[i] != (i < 60 ? 0 : 0x5A))
            return false;
    return true;
}

// Textures/CryMT/ssrpart.dds: CryMT's 2x1 placeholder for the previous frame's particle layer (the water's SSR): the first
// texel zero, the second 4F4F4F4F (the mark).
bool IsSsrPartPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x4F))
            return false;
    return true;
}

// Textures/CryMT/ssrprev.dds: CryMT's 2x1 placeholder for the previous frame's scene (the water's SSR, patched Water.cfx):
// the first texel zero, the second 4E4E4E4E (the mark).
bool IsSsrPrevPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x4E))
            return false;
    return true;
}

// Textures/CryMT/foliage.dds: CryMT's 2x1 placeholder for the foliage mask (patched ShadowMaskGen.cfx): the first
// texel zero, the second A5A5A5A5 (the mark).
bool IsFoliagePlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0xA5))
            return false;
    return true;
}

// Textures/CryMT/pvol.dds: CryMT's 2x1 placeholder for the particles' occlusion volume (patched Particles.cfi): the
// first texel zero (a cell size of 0: no change), the second 3C3C3C3C (the mark).
bool IsPvolPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x3C))
            return false;
    return true;
}

// Textures/CryMT/vfog.dds: CryMT's 4x1x2 volume of zeros for the GI in the fog (patched PostProcess.cfx: zeros leave the
// engine's fog as it is). Told by its size and format (its texels too when the engine hands them over).
bool IsVfogPlaceholder(const D3D10_TEXTURE3D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (d.Width != 4 || d.Height != 1 || d.Depth != 2 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    if (init && init->pSysMem)
        for (UINT z = 0; z < 2; z++)
            for (UINT i = 0; i < 16; i++)
                if (static_cast<const uint8_t*>(init->pSysMem)[z * init->SysMemSlicePitch + i])
                    return false;
    return true;
}

// Textures/CryMT/fplus.dds: CryMT's 2x1 placeholder for the Forward+ test's tile lists (patched fragLib.cfi): the first
// texel zero (off), the second 6B6B6B6B (the mark).
bool IsFPlusPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x6B))
            return false;
    return true;
}

// Textures/CryMT/sss.dds: CryMT's 2x1 placeholder for the screen-space shadows (patched ShadowMaskGen.cfx, fragLib.cfi):
// the first texel zero (no shadow), the second 005C005C (the mark; red zero in either byte order).
bool IsSssPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i == 5 || i == 7 ? 0x5C : 0))
            return false;
    return true;
}

// Textures/CryMT/hiz.dds: CryMT's 2x1 placeholder for the water's depth pyramid (patched Water.cfx): red zero in both
// texels (no surface), the second texel 007C007C (the mark).
bool IsHizPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i == 5 || i == 7 ? 0x7C : 0))
            return false;
    return true;
}

// Textures/CryMT/waterenv.dds: CryMT's placeholder for the water's reflection cube (patched Water.cfx): a 2x2 cube, alpha 0
// everywhere, face 0's second texel 003D3D3D (the mark).
bool IsWaterEnvPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 2 || d.ArraySize != 6 || !(d.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i >= 4 && i < 7 ? 0x3D : 0))
            return false;
    return true;
}

HRESULT Device::CreateTexture2D(const D3D10_TEXTURE2D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture2D** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* t = new Texture2D12;
    FillTexture(t, *desc, D3D10_RESOURCE_DIMENSION_TEXTURE2D, desc->Width, desc->Height, desc->ArraySize, desc->SampleDesc.Count, desc->SampleDesc.Quality);
    const HRESULT hr = FinishTexture(t, init);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    if (IsEnvPlaceholder(*desc, init))
    {
        g_envPlaceholder = Info(t);
        Log("environment placeholder texture recognized");
    }
    else if (IsWaterEnvPlaceholder(*desc, init))
    {
        g_waterEnvPlaceholder = Info(t);
        Log("water reflection cube placeholder texture recognized");
    }
    else if (IsEnvCube(*desc, init))
    {
        Info(t)->envCube = true;
        static int logged;
        if (logged++ < 64)
            Log("environment cube %u: %ux%u, %u mips, format %d", logged, desc->Width, desc->Height, desc->MipLevels, static_cast<int>(desc->Format));
    }
    else if (desc->MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE)
    {
        static int logged;
        if (logged++ < 32)
            Log("other cube texture: %ux%u, %u mips, format %d, bind 0x%x, usage %d, data %d", desc->Width, desc->Height, desc->MipLevels,
                static_cast<int>(desc->Format), desc->BindFlags, static_cast<int>(desc->Usage), init && init->pSysMem ? 1 : 0);
    }
    if (IsGiPlaceholder(*desc, init))
    {
        g_giPlaceholder = Info(t);
        Log("GI placeholder texture recognized");
    }
    else if (IsJitterPlaceholder(*desc, init))
    {
        g_jitterPlaceholder = Info(t);
        Log("jitter placeholder texture recognized");
    }
    else if (IsFoliagePlaceholder(*desc, init))
    {
        g_foliagePlaceholder = Info(t);
        Log("foliage placeholder texture recognized");
    }
    else if (IsPvolPlaceholder(*desc, init))
    {
        g_pvolPlaceholder = Info(t);
        Log("particle volume placeholder texture recognized");
    }
    else if (IsFPlusPlaceholder(*desc, init))
    {
        g_fplusPlaceholder = Info(t);
        Log("Forward+ placeholder texture recognized");
    }
    else if (IsSssPlaceholder(*desc, init))
    {
        g_sssPlaceholder = Info(t);
        Log("screen-space shadows placeholder texture recognized");
    }
    else if (IsHizPlaceholder(*desc, init))
    {
        g_hizPlaceholder = Info(t);
        Log("depth pyramid placeholder texture recognized");
    }
    else if (IsSsrPrevPlaceholder(*desc, init))
    {
        g_ssrPrevPlaceholder = Info(t);
        Log("water previous-frame placeholder texture recognized");
    }
    else if (IsSsrPartPlaceholder(*desc, init))
    {
        g_ssrPartPlaceholder = Info(t);
        Log("water particle-layer placeholder texture recognized");
    }
    else if (desc->Width == 4 && desc->Height <= 4 && Gi().on)
    {
        int zero = -1;
        if (init && init->pSysMem && (desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc->Format == DXGI_FORMAT_B8G8R8A8_UNORM))
        {
            zero = 1;
            for (UINT y = 0; y < desc->Height; y++)
                for (UINT x = 0; x < 16; x++)
                    if (static_cast<const uint8_t*>(init->pSysMem)[y * init->SysMemPitch + x])
                        zero = 0;
        }
        Log("small texture %ux%u fmt %d mips %u usage %d bind 0x%x init %d zero %d", desc->Width, desc->Height, desc->Format, desc->MipLevels,
            desc->Usage, desc->BindFlags, init && init->pSysMem ? 1 : 0, zero);
    }
    t->desc.MipLevels = t->mips;
    *out = t;
    return S_OK;
}

HRESULT Device::CreateTexture3D(const D3D10_TEXTURE3D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture3D** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* t = new Texture3D12;
    FillTexture(t, *desc, D3D10_RESOURCE_DIMENSION_TEXTURE3D, desc->Width, desc->Height, desc->Depth, 1, 0);
    const HRESULT hr = FinishTexture(t, init);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    if (IsVfogPlaceholder(*desc, init))
    {
        g_vfogPlaceholder = Info(t);
        Log("volumetric fog placeholder texture recognized");
    }
    t->desc.MipLevels = t->mips;
    *out = t;
    return S_OK;
}

// The view D3D10 makes when no description is given: the whole resource in its own format.
D3D10_SHADER_RESOURCE_VIEW_DESC1 DefaultSrv(const TextureCore* t)
{
    D3D10_SHADER_RESOURCE_VIEW_DESC1 d{};
    d.Format = t->format;
    const UINT arrays = t->depthOrArray;
    int dim;
    if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
    {
        dim = arrays > 1 ? D3D10_SRV_DIMENSION_TEXTURE1DARRAY : D3D10_SRV_DIMENSION_TEXTURE1D;
        d.Texture1DArray = {0, t->mips, 0, arrays};
    }
    else if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D)
    {
        dim = D3D10_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D = {0, t->mips};
    }
    else if (t->samples > 1)
    {
        dim = arrays > 1 ? D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D10_SRV_DIMENSION_TEXTURE2DMS;
        d.Texture2DMSArray = {0, arrays};
    }
    else if (t->misc & D3D10_RESOURCE_MISC_TEXTURECUBE)
    {
        dim = arrays > 6 ? D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY : D3D10_SRV_DIMENSION_TEXTURECUBE;
        d.TextureCubeArray = {0, t->mips, 0, arrays / 6};
    }
    else
    {
        dim = arrays > 1 ? D3D10_SRV_DIMENSION_TEXTURE2DARRAY : D3D10_SRV_DIMENSION_TEXTURE2D;
        d.Texture2DArray = {0, t->mips, 0, arrays};
    }
    d.ViewDimension = static_cast<D3D10_SRV_DIMENSION1>(dim);
    return d;
}

HRESULT Device::CreateShaderResourceView1(ID3D10Resource* r, const D3D10_SHADER_RESOURCE_VIEW_DESC1* desc, ID3D10ShaderResourceView1** out)
{
    ResourceInfo* info = Info(r);
    if (!info || (info->buffer && !desc))
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    const D3D10_SHADER_RESOURCE_VIEW_DESC1 d1 = desc ? *desc : DefaultSrv(static_cast<TextureCore*>(info));
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = d1.Format;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.ViewDimension = static_cast<D3D12_SRV_DIMENSION>(d1.ViewDimension);
    switch (static_cast<int>(d1.ViewDimension))
    {
    case D3D10_SRV_DIMENSION_BUFFER:
        d.Buffer.FirstElement = d1.Buffer.FirstElement;
        d.Buffer.NumElements = d1.Buffer.NumElements;
        break;
    case D3D10_SRV_DIMENSION_TEXTURE1D:
        d.Texture1D = {d1.Texture1D.MostDetailedMip, d1.Texture1D.MipLevels, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE1DARRAY:
        d.Texture1DArray = {d1.Texture1DArray.MostDetailedMip, d1.Texture1DArray.MipLevels, d1.Texture1DArray.FirstArraySlice, d1.Texture1DArray.ArraySize, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2D:
        d.Texture2D = {d1.Texture2D.MostDetailedMip, d1.Texture2D.MipLevels, 0, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2DARRAY:
        d.Texture2DArray = {d1.Texture2DArray.MostDetailedMip, d1.Texture2DArray.MipLevels, d1.Texture2DArray.FirstArraySlice, d1.Texture2DArray.ArraySize, 0,
                            0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY:
        d.Texture2DMSArray = {d1.Texture2DMSArray.FirstArraySlice, d1.Texture2DMSArray.ArraySize};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE3D:
        d.Texture3D = {d1.Texture3D.MostDetailedMip, d1.Texture3D.MipLevels, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURECUBE:
        d.TextureCube = {d1.TextureCube.MostDetailedMip, d1.TextureCube.MipLevels, 0.0f};
        break;
    case D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY:
        d.TextureCubeArray = {d1.TextureCubeArray.MostDetailedMip, d1.TextureCubeArray.MipLevels, d1.TextureCubeArray.First2DArrayFace,
                              d1.TextureCubeArray.NumCubes, 0.0f};
        break;
    default:
        return E_INVALIDARG;
    }
    auto* v = new Srv12;
    r->AddRef();
    v->resource = r;
    v->info = info;
    v->desc = d1;
    v->d12 = d;
    v->heap = &g_srvCpu;
    v->cpu = g_srvCpu.Alloc();
    if (info->buffer)
    {
        auto* b = static_cast<Buffer12*>(info);
        D3D12_SHADER_RESOURCE_VIEW_DESC bd = d;
        bd.Buffer.FirstElement += b->Offset() / FormatBytes(d.Format);
        g.dev->CreateShaderResourceView(b->Res(), &bd, v->cpu);
        v->generation = b->generation;
    }
    else
        g.dev->CreateShaderResourceView(info->res, &d, v->cpu);
    *out = v;
    return S_OK;
}

HRESULT Device::CreateRenderTargetView(ID3D10Resource* r, const D3D10_RENDER_TARGET_VIEW_DESC* desc, ID3D10RenderTargetView** out)
{
    TextureCore* t = r ? AsTexture(r) : nullptr;
    if (!t)
    {
        LogLimited("rtvbuf", "render target views of buffers are not supported");
        return E_INVALIDARG;
    }
    if (!out)
        return S_FALSE;
    D3D10_RENDER_TARGET_VIEW_DESC d1{};
    if (desc)
        d1 = *desc;
    else
    {
        d1.Format = t->format;
        if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_RTV_DIMENSION_TEXTURE1DARRAY : D3D10_RTV_DIMENSION_TEXTURE1D, d1.Texture1DArray = {0, 0, t->depthOrArray};
        else if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D)
            d1.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE3D, d1.Texture3D = {0, 0, t->depthOrArray};
        else if (t->samples > 1)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D10_RTV_DIMENSION_TEXTURE2DMS,
            d1.Texture2DMSArray = {0, t->depthOrArray};
        else
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_RTV_DIMENSION_TEXTURE2DARRAY : D3D10_RTV_DIMENSION_TEXTURE2D, d1.Texture2DArray = {0, 0, t->depthOrArray};
    }
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = d1.Format;
    d.ViewDimension = static_cast<D3D12_RTV_DIMENSION>(d1.ViewDimension);
    switch (d1.ViewDimension)
    {
    case D3D10_RTV_DIMENSION_TEXTURE1D:
        d.Texture1D.MipSlice = d1.Texture1D.MipSlice;
        break;
    case D3D10_RTV_DIMENSION_TEXTURE1DARRAY:
        d.Texture1DArray = {d1.Texture1DArray.MipSlice, d1.Texture1DArray.FirstArraySlice, d1.Texture1DArray.ArraySize};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2D:
        d.Texture2D = {d1.Texture2D.MipSlice, 0};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2DARRAY:
        d.Texture2DArray = {d1.Texture2DArray.MipSlice, d1.Texture2DArray.FirstArraySlice, d1.Texture2DArray.ArraySize, 0};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY:
        d.Texture2DMSArray = {d1.Texture2DMSArray.FirstArraySlice, d1.Texture2DMSArray.ArraySize};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE3D:
        d.Texture3D = {d1.Texture3D.MipSlice, d1.Texture3D.FirstWSlice, d1.Texture3D.WSize};
        break;
    default:
        return E_INVALIDARG;
    }
    auto* v = new Rtv12;
    r->AddRef();
    v->resource = r;
    v->info = t;
    v->desc = d1;
    v->format = d1.Format != DXGI_FORMAT_UNKNOWN ? d1.Format : t->format;
    v->samples = t->samples;
    v->quality = t->quality;
    v->heap = &g_rtvCpu;
    v->cpu = g_rtvCpu.Alloc();
    g.dev->CreateRenderTargetView(t->res, &d, v->cpu);
    *out = v;
    return S_OK;
}

HRESULT Device::CreateDepthStencilView(ID3D10Resource* r, const D3D10_DEPTH_STENCIL_VIEW_DESC* desc, ID3D10DepthStencilView** out)
{
    TextureCore* t = r ? AsTexture(r) : nullptr;
    if (!t)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    D3D10_DEPTH_STENCIL_VIEW_DESC d1{};
    if (desc)
        d1 = *desc;
    else
    {
        d1.Format = t->format;
        if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_DSV_DIMENSION_TEXTURE1DARRAY : D3D10_DSV_DIMENSION_TEXTURE1D, d1.Texture1DArray = {0, 0, t->depthOrArray};
        else if (t->samples > 1)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D10_DSV_DIMENSION_TEXTURE2DMS,
            d1.Texture2DMSArray = {0, t->depthOrArray};
        else
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_DSV_DIMENSION_TEXTURE2DARRAY : D3D10_DSV_DIMENSION_TEXTURE2D, d1.Texture2DArray = {0, 0, t->depthOrArray};
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = d1.Format;
    d.ViewDimension = static_cast<D3D12_DSV_DIMENSION>(d1.ViewDimension);
    switch (d1.ViewDimension)
    {
    case D3D10_DSV_DIMENSION_TEXTURE1D:
        d.Texture1D.MipSlice = d1.Texture1D.MipSlice;
        break;
    case D3D10_DSV_DIMENSION_TEXTURE1DARRAY:
        d.Texture1DArray = {d1.Texture1DArray.MipSlice, d1.Texture1DArray.FirstArraySlice, d1.Texture1DArray.ArraySize};
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2D:
        d.Texture2D.MipSlice = d1.Texture2D.MipSlice;
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2DARRAY:
        d.Texture2DArray = {d1.Texture2DArray.MipSlice, d1.Texture2DArray.FirstArraySlice, d1.Texture2DArray.ArraySize};
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY:
        d.Texture2DMSArray = {d1.Texture2DMSArray.FirstArraySlice, d1.Texture2DMSArray.ArraySize};
        break;
    default:
        return E_INVALIDARG;
    }
    auto* v = new Dsv12;
    r->AddRef();
    v->resource = r;
    v->info = t;
    v->desc = d1;
    v->format = d1.Format != DXGI_FORMAT_UNKNOWN ? d1.Format : t->format;
    v->samples = t->samples;
    v->quality = t->quality;
    v->heap = &g_dsvCpu;
    v->cpu = g_dsvCpu.Alloc();
    g.dev->CreateDepthStencilView(t->res, &d, v->cpu);
    *out = v;
    return S_OK;
}

HRESULT Device::CreateInputLayout(const D3D10_INPUT_ELEMENT_DESC* elements, UINT n, const void*, SIZE_T, ID3D10InputLayout** out)
{
    if (!elements && n)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* l = new Layout12;
    for (UINT i = 0; i < n; i++)
    {
        const D3D10_INPUT_ELEMENT_DESC& e = elements[i];
        l->names.emplace_back(e.SemanticName ? e.SemanticName : "");
        l->elements.push_back({l->names.back().c_str(), e.SemanticIndex, e.Format, e.InputSlot, e.AlignedByteOffset,
                               static_cast<D3D12_INPUT_CLASSIFICATION>(e.InputSlotClass), e.InstanceDataStepRate});
        l->slotEnd = std::max(l->slotEnd, std::min(e.InputSlot + 1, 16u));
        l->animated = l->animated || e.InputSlotClass == D3D10_INPUT_PER_INSTANCE_DATA || _strnicmp(l->names.back().c_str(), "BLEND", 5) == 0;
        l->contentHash = HashBytes(l->names.back().data(), l->names.back().size(), l->contentHash ? l->contentHash : 1469598103934665603ull);
        const UINT fields[6] = {e.SemanticIndex, static_cast<UINT>(e.Format), e.InputSlot, e.AlignedByteOffset, static_cast<UINT>(e.InputSlotClass),
                                e.InstanceDataStepRate};
        l->contentHash = HashBytes(fields, sizeof(fields), l->contentHash);
    }
    *out = l;
    return S_OK;
}

template <class T, class Init>
HRESULT CreateState(StateCache<T>& cache, const void* desc, size_t bytes, T** out, Init init)
{
    std::string key(static_cast<const char*>(desc), bytes);
    std::lock_guard<std::mutex> guard(g_stateLock);
    if (T* found = cache.Find(key))
    {
        *out = found;
        return S_OK;
    }
    auto* s = new T;
    s->key = key;
    s->cache = &cache;
    init(s);
    cache.map[key] = s;
    *out = s;
    return S_OK;
}

HRESULT Device::CreateBlendState1(const D3D10_BLEND_DESC1* desc, ID3D10BlendState1** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Blend12* b = nullptr;
    CreateState(g_blendCache, desc, sizeof(*desc), &b, [desc](Blend12* s) {
        s->desc = *desc;
        s->d12 = Blend12From(*desc);
    });
    *out = b;
    return S_OK;
}

HRESULT Device::CreateDepthStencilState(const D3D10_DEPTH_STENCIL_DESC* desc, ID3D10DepthStencilState** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Depth12* d = nullptr;
    CreateState(g_depthCache, desc, sizeof(*desc), &d, [desc](Depth12* s) {
        s->desc = *desc;
        s->d12 = Depth12From(*desc);
    });
    *out = d;
    return S_OK;
}

HRESULT Device::CreateRasterizerState(const D3D10_RASTERIZER_DESC* desc, ID3D10RasterizerState** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Raster12* r = nullptr;
    CreateState(g_rasterCache, desc, sizeof(*desc), &r, [desc](Raster12* s) {
        s->desc = *desc;
        s->d12 = Raster12From(*desc);
    });
    *out = r;
    return S_OK;
}

HRESULT Device::CreateSamplerState(const D3D10_SAMPLER_DESC* desc, ID3D10SamplerState** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Sampler12* smp = nullptr;
    CreateState(g_samplerCache, desc, sizeof(*desc), &smp, [desc](Sampler12* s) {
        s->desc = *desc;
        D3D12_SAMPLER_DESC d{};
        d.Filter = static_cast<D3D12_FILTER>(desc->Filter & 0x7FFFFFFF);
        d.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc->AddressU);
        d.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc->AddressV);
        d.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc->AddressW);
        // CRYMT_MIPBIAS (tests): added to every sampler's mip bias (DLSS's guide: a bias against shimmering patterns or
        // for detail).
        static const float extraBias = [] {
            char text[32] = {};
            return GetEnvironmentVariableA("CRYMT_MIPBIAS", text, sizeof(text)) ? static_cast<float>(atof(text)) : 0.0f;
        }();
        d.MipLODBias = desc->MipLODBias + (desc->ComparisonFunc ? 0.0f : extraBias);
        d.MaxAnisotropy = std::min(16u, std::max(1u, desc->MaxAnisotropy));
        d.ComparisonFunc = desc->ComparisonFunc ? static_cast<D3D12_COMPARISON_FUNC>(desc->ComparisonFunc) : D3D12_COMPARISON_FUNC_NEVER;
        memcpy(d.BorderColor, desc->BorderColor, sizeof(d.BorderColor));
        d.MinLOD = desc->MinLOD;
        d.MaxLOD = desc->MaxLOD;
        s->cpu = g_samplerCpu.Alloc();
        g.dev->CreateSampler(&d, s->cpu);
    });
    *out = smp;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queries: occlusion counts and timestamps resolve into a readback buffer when they end; events are the end of a list.
// A query must begin and end in one command list, so active occlusion queries end and restart around submissions.
// ---------------------------------------------------------------------------------------------------------------------
std::vector<Query12*> g_activeQueries;

bool OcclusionQuery(const Query12* q)
{
    return q->desc.Query == D3D10_QUERY_OCCLUSION || q->desc.Query == D3D10_QUERY_OCCLUSION_PREDICATE;
}

D3D12_QUERY_TYPE OcclusionType(const Query12* q)
{
    return q->desc.Query == D3D10_QUERY_OCCLUSION_PREDICATE ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION;
}

HRESULT Device::CreateQuery(const D3D10_QUERY_DESC* desc, ID3D10Query** out)
{
    if (!desc)
        return E_INVALIDARG;
    switch (desc->Query)
    {
    case D3D10_QUERY_EVENT:
    case D3D10_QUERY_OCCLUSION:
    case D3D10_QUERY_OCCLUSION_PREDICATE:
    case D3D10_QUERY_TIMESTAMP:
    case D3D10_QUERY_TIMESTAMP_DISJOINT:
        break;
    default:
        LogLimited("querytype", "query type %d is not supported", desc->Query);
        return E_NOTIMPL;
    }
    if (!out)
        return S_FALSE;
    auto* q = new Query12;
    q->desc = *desc;
    if (OcclusionQuery(q) || desc->Query == D3D10_QUERY_TIMESTAMP)
    {
        std::lock_guard<std::mutex> guard(m_queryLock);
        if (!m_queryReadback)
        {
            D3D12_QUERY_HEAP_DESC hd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQuerySlots, 0};
            g.dev->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_occlusionHeap));
            hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            g.dev->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_timestampHeap));
            // Final occlusion counts, timestamps, partial occlusion counts.
            m_queryReadback = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, 3 * kQuerySlots * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST);
            void* p = nullptr;
            m_queryReadback->Map(0, nullptr, &p);
            m_queryCpu = static_cast<const uint64_t*>(p);
            for (int i = kQuerySlots - 1; i >= 0; i--)
            {
                m_freeOcclusion.push_back(i);
                m_freeTimestamp.push_back(i);
            }
        }
        std::vector<int>& free = OcclusionQuery(q) ? m_freeOcclusion : m_freeTimestamp;
        if (free.empty())
        {
            q->Release();
            LogLimited("queryfull", "out of query slots");
            return E_OUTOFMEMORY;
        }
        q->heapIndex = free.back();
        free.pop_back();
    }
    *out = q;
    return S_OK;
}

void Device::QueryBegin(Query12* q)
{
    if (!OcclusionQuery(q))
        return;
    if (std::find(g_activeQueries.begin(), g_activeQueries.end(), q) != g_activeQueries.end())
        return;
    m_list->BeginQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex);
    q->split = false;
    g_activeQueries.push_back(q);
    m_activeQueries = g_activeQueries.size();
}

// Around a submission: active occlusion queries end in the old list (their count so far resolved to the partial slot)
// and begin again in the new one.
void Device::SplitQueries(bool end)
{
    for (Query12* q : g_activeQueries)
    {
        const D3D12_QUERY_TYPE type = OcclusionType(q);
        if (!end)
        {
            m_list->BeginQuery(m_occlusionHeap, type, q->heapIndex);
            continue;
        }
        if (q->split)
            LogLimited("split2", "an occlusion query spans more than two submissions; its first part is lost");
        m_list->EndQuery(m_occlusionHeap, type, q->heapIndex);
        m_list->ResolveQueryData(m_occlusionHeap, type, q->heapIndex, 1, m_queryReadback, (2 * kQuerySlots + q->heapIndex) * sizeof(uint64_t));
        q->split = true;
    }
}

void Device::QueryEnd(Query12* q)
{
    if (OcclusionQuery(q))
    {
        const auto it = std::find(g_activeQueries.begin(), g_activeQueries.end(), q);
        if (it == g_activeQueries.end())
        {
            m_list->BeginQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex); // ended without a begin: an empty count
            q->split = false;
        }
        else
            g_activeQueries.erase(it);
        m_activeQueries = g_activeQueries.size();
        m_list->EndQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex);
        m_list->ResolveQueryData(m_occlusionHeap, OcclusionType(q), q->heapIndex, 1, m_queryReadback, q->heapIndex * sizeof(uint64_t));
    }
    else if (q->desc.Query == D3D10_QUERY_TIMESTAMP)
    {
        m_list->EndQuery(m_timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q->heapIndex);
        m_list->ResolveQueryData(m_timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q->heapIndex, 1, m_queryReadback,
                                 (kQuerySlots + q->heapIndex) * sizeof(uint64_t));
    }
    q->serial = g_mainSerial;
    q->ended = true;
}

HRESULT Device::QueryData(Query12* q, void* data, UINT size, UINT flags)
{
    if (!q->ended)
        return S_FALSE;
    if (q->serial >= g_mainSerial)
    {
        if (!(flags & D3D10_ASYNC_GETDATA_DONOTFLUSH))
            Submit();
        return S_FALSE;
    }
    PollDone();
    if (!SerialDone(q->serial))
        return S_FALSE;
    return QueryRead(q, data, size);
}

// The producer's GetData (D3DLayer) after the stream thread executed the query's End: no submit (the stream thread does
// that, Dx12QueryFlush) and the completed serial refreshed only when the lock is free.
HRESULT Device::QueryPeek(Query12* q, void* data, UINT size, UINT flags, uint64_t* flushSerial)
{
    *flushSerial = 0;
    if (!q->ended)
        return S_FALSE;
    if (q->serial >= __atomic_load_n(&g_mainSerial, __ATOMIC_ACQUIRE))
    {
        if (!(flags & D3D10_ASYNC_GETDATA_DONOTFLUSH))
            *flushSerial = q->serial;
        return S_FALSE;
    }
    if (!SerialDone(q->serial) && g.lock.try_lock())
    {
        Retire();
        g.lock.unlock();
    }
    if (!SerialDone(q->serial))
        return S_FALSE;
    return QueryRead(q, data, size);
}

HRESULT Device::QueryRead(Query12* q, void* data, UINT size)
{
    if (!data)
        return S_OK;
    if (size != q->GetDataSize())
        return E_INVALIDARG;
    switch (q->desc.Query)
    {
    case D3D10_QUERY_EVENT:
        *static_cast<BOOL*>(data) = TRUE;
        break;
    case D3D10_QUERY_OCCLUSION:
        *static_cast<UINT64*>(data) = m_queryCpu[q->heapIndex] + (q->split ? m_queryCpu[2 * kQuerySlots + q->heapIndex] : 0);
        break;
    case D3D10_QUERY_OCCLUSION_PREDICATE:
        *static_cast<BOOL*>(data) = m_queryCpu[q->heapIndex] != 0 || (q->split && m_queryCpu[2 * kQuerySlots + q->heapIndex] != 0);
        break;
    case D3D10_QUERY_TIMESTAMP:
        *static_cast<UINT64*>(data) = m_queryCpu[kQuerySlots + q->heapIndex];
        break;
    case D3D10_QUERY_TIMESTAMP_DISJOINT:
    {
        UINT64 frequency = 0;
        g.queue->GetTimestampFrequency(&frequency);
        *static_cast<D3D10_QUERY_DATA_TIMESTAMP_DISJOINT*>(data) = {frequency, FALSE};
        break;
    }
    default:
        return E_FAIL;
    }
    return S_OK;
}

void Device::QueryFree(Query12* q)
{
    const auto it = std::find(g_activeQueries.begin(), g_activeQueries.end(), q);
    if (it != g_activeQueries.end())
    {
        m_list->EndQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex); // released while active
        g_activeQueries.erase(it);
        m_activeQueries = g_activeQueries.size();
    }
    if (q->heapIndex < 0)
        return;
    const int index = q->heapIndex;
    const bool occlusion = OcclusionQuery(q);
    DeferCall([this, index, occlusion] {
        std::lock_guard<std::mutex> guard(m_queryLock);
        (occlusion ? m_freeOcclusion : m_freeTimestamp).push_back(index);
    });
}

void STDMETHODCALLTYPE Query12::Begin()
{
    g_device->QueryBegin(this);
}

void STDMETHODCALLTYPE Query12::End()
{
    g_device->QueryEnd(this);
}

HRESULT STDMETHODCALLTYPE Query12::GetData(void* data, UINT size, UINT flags)
{
    return g_device->QueryData(this, data, size, flags);
}

void Query12::Destroy()
{
    g_device->QueryFree(this);
    Child::Destroy();
}

// ---------------------------------------------------------------------------------------------------------------------
// Presentation: the renderer draws into our back buffer texture; Present copies it into the flip-model swap chain's
// current buffer. At most kFramesInFlight frames are queued.
// ---------------------------------------------------------------------------------------------------------------------
// Frame capture for image comparisons (CRYMT_CAPTURE_SEC=seconds after start, CRYMT_CAPTURE_COUNT, CRYMT_CAPTURE_TAG):
// consecutive back buffers copied to readback memory and written as PPM files to logs\play, without touching the window.
// CRYMT_CAPTURE_SEC may list several windows ("45,60"); window w writes <tag>_w<w>_c<n>.ppm. CRYMT_CAPTURE_FRAME lists
// world frames instead (CryMT's frame counter: a timedemo shows the same camera at the same frame in every run).
struct Capture
{
    std::vector<DWORD> starts;
    bool byFrame = false;
    size_t window = 0;
    int count = 0, done = 0;
    char tag[64] = "capture";
    std::vector<ID3D12Resource*> buffers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
};
Capture g_capture;
// The capture key (CryMT.cpp, PgUp with CRYMT_RECORDKEYS=auto): main asks for that many frames from the next present;
// they go to logs\play\key_MMDD_HHMMSS_w1_c<n>.ppm. Ignored while a capture runs.
std::atomic<int> g_captureKeyFrames{0};
std::atomic<int> g_captureKeyFlags{0}; // what was on, for the name: 1 GI, 2 the DLAA jitter, 4 object velocities
// Scene hand-overs left to dump DLSS's input and output (the capture key: 1; with CRYMT_MVSEQ=1 every captured frame, for
// the motion vector check tools\prof\mv_check.py: colour, vectors, the engine's object velocities, the foliage mask).
std::atomic<int> g_sceneDumpFrames{0};
int MvSequenceLevel()
{
    static const int level = [] {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"CRYMT_MVSEQ", v, 8) ? _wtoi(v) : 0;
    }();
    return level;
}
bool MvSequence()
{
    return MvSequenceLevel() != 0;
}

// Milliseconds since the process started (capture windows line up with CryMT's comparison schedule).
DWORD ProcessMilliseconds()
{
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<DWORD>((ticks(now) - ticks(created)) / 10000);
}

void CaptureInit()
{
    wchar_t text[256];
    if (GetEnvironmentVariableW(L"CRYMT_CAPTURE_TAG", text, 64))
        snprintf(g_capture.tag, sizeof(g_capture.tag), "%ls", text);
    g_capture.byFrame = GetEnvironmentVariableW(L"CRYMT_CAPTURE_FRAME", text, 256) != 0;
    if (!g_capture.byFrame && !GetEnvironmentVariableW(L"CRYMT_CAPTURE_SEC", text, 256))
        return;
    for (const wchar_t* p = text; *p;)
    {
        g_capture.starts.push_back(static_cast<DWORD>(_wtof(p) * (g_capture.byFrame ? 1.0 : 1000.0)));
        while (*p && *p != L',')
            p++;
        if (*p == L',')
            p++;
    }
    g_capture.count = GetEnvironmentVariableW(L"CRYMT_CAPTURE_COUNT", text, 64) ? _wtoi(text) : 8;
}

// Every frame, reduced (CRYMT_CAPTURE_STREAM=n: 1/n of the size each way, box filter), appended to
// logs\play\<tag>_stream.raw: width and height (uint32), then per frame the world frame (uint32) and RGB rows. For
// counting one-frame flicker over a whole demo; a frame is read back kFramesInFlight frames later, when it is done.
struct CaptureStream
{
    int scale = 0;
    FILE* file = nullptr;
    UINT width = 0, height = 0, written = 0;
    ID3D12Resource* buffers[kFramesInFlight] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kFramesInFlight] = {};
    uint32_t worldFrames[kFramesInFlight] = {};
    bool filled[kFramesInFlight] = {};
};
CaptureStream g_stream;

void CaptureStreamInit()
{
    wchar_t text[16];
    if (GetEnvironmentVariableW(L"CRYMT_CAPTURE_STREAM", text, 16))
        g_stream.scale = std::max(1, _wtoi(text));
}

void CaptureStreamWrite(size_t slot)
{
    CaptureStream& s = g_stream;
    s.filled[slot] = false;
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = s.footprints[slot];
    const UINT w = fp.Footprint.Width / s.scale, h = fp.Footprint.Height / s.scale;
    if (!s.file)
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s%s_stream.raw", CryMTCapture("").c_str(), g_capture.tag);
        if (fopen_s(&s.file, path, "wb") != 0 || !s.file)
        {
            s.scale = 0;
            return;
        }
        s.width = w;
        s.height = h;
        fwrite(&s.width, 4, 1, s.file);
        fwrite(&s.height, 4, 1, s.file);
    }
    void* p = nullptr;
    if (w != s.width || h != s.height || FAILED(s.buffers[slot]->Map(0, nullptr, &p)) || !p)
        return;
    const bool bgra = fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const UINT n = static_cast<UINT>(s.scale), area = n * n;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3);
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++)
        {
            UINT sum[3] = {};
            for (UINT dy = 0; dy < n; dy++)
            {
                const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y * n + dy) * fp.Footprint.RowPitch + x * n * 4;
                for (UINT dx = 0; dx < n; dx++, src += 4)
                {
                    sum[0] += src[bgra ? 2 : 0];
                    sum[1] += src[1];
                    sum[2] += src[bgra ? 0 : 2];
                }
            }
            uint8_t* dst = &out[(static_cast<size_t>(y) * w + x) * 3];
            for (int c = 0; c < 3; c++)
                dst[c] = static_cast<uint8_t>(sum[c] / area);
        }
    s.buffers[slot]->Unmap(0, nullptr);
    fwrite(&s.worldFrames[slot], 4, 1, s.file);
    fwrite(out.data(), 1, out.size(), s.file);
    if (++s.written % 64 == 0)
        fflush(s.file);
}

void CaptureWrite(int index)
{
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = g_capture.footprints[index];
    void* p = nullptr;
    g_capture.buffers[index]->Map(0, nullptr, &p);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s_w%zu_c%d.ppm", CryMTCapture("").c_str(), g_capture.tag, g_capture.window + 1, index + 1);
    FILE* f = nullptr;
    if (p && fopen_s(&f, path, "wb") == 0 && f)
    {
        fprintf(f, "P6\n%u %u\n255\n", fp.Footprint.Width, fp.Footprint.Height);
        std::vector<uint8_t> row(fp.Footprint.Width * 3);
        const bool bgra = fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        for (UINT y = 0; y < fp.Footprint.Height; y++)
        {
            const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * fp.Footprint.RowPitch;
            for (UINT x = 0; x < fp.Footprint.Width; x++)
            {
                row[x * 3] = src[x * 4 + (bgra ? 2 : 0)];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + (bgra ? 0 : 2)];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    g_capture.buffers[index]->Unmap(0, nullptr);
}

HRESULT Device::PresentFrame(IDXGISwapChain3* chain, ID3D12Resource* target, TextureCore* backbuffer, UINT sync, UINT flags)
{
    m_note.frame = m_frame;
    m_notes[m_frame % 16] = m_note;
    if (CopyLog() && m_frame % 600 == 300)
    {
        Log("pre-DLSS copies, frame %llu: %u (hand-over in [%s])", static_cast<unsigned long long>(m_frame), m_preCopyCount, m_handOverPass.c_str());
        for (uint32_t i = 0; i < m_preCopyCount; i++)
        {
            const auto* t = static_cast<const TextureCore*>(m_preCopies[i]);
            Log("  copy %u: %p %ux%u fmt %d, %s%s", i, static_cast<const void*>(t), t->width, t->height, static_cast<int>(t->format),
                m_preCopyWhere[i].c_str(), m_preCopyReads[i].empty() ? "\n    not read after DLSS" : m_preCopyReads[i].c_str());
        }
    }
    m_note = FrameNote{};
    m_preCopyCount = 0;
    GiApplyControl();
    RunVoxelView(backbuffer);
    if (g_stream.scale)
    {
        // This slot's last frame is done (the wait at the end of the present kFramesInFlight - 1 frames ago).
        const size_t slot = m_frame % kFramesInFlight;
        if (g_stream.filled[slot])
            CaptureStreamWrite(slot);
        D3D12_RESOURCE_DESC rd = backbuffer->Desc12();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = g_stream.footprints[slot];
        if (g_stream.buffers[slot] && (fp.Footprint.Width != rd.Width || fp.Footprint.Height != rd.Height))
        {
            g_stream.buffers[slot]->Release();
            g_stream.buffers[slot] = nullptr;
        }
        if (!g_stream.buffers[slot])
        {
            UINT64 total = 0;
            g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            g_stream.buffers[slot] = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (g_stream.buffers[slot] && (fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM || fp.Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM))
        {
            Require(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_stream.buffers[slot];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = backbuffer->res;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            NoteCopy("stream capture", dst, 0, 0, 0, src, nullptr);
            m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            g_stream.worldFrames[slot] = static_cast<uint32_t>(::CryMTWorldFrame());
            g_stream.filled[slot] = true;
        }
    }
    if (const int keyFrames = g_captureKeyFrames.exchange(0); keyFrames > 0 && g_capture.done == 0 && g_capture.buffers.empty())
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        const int flags = g_captureKeyFlags.load();
        snprintf(g_capture.tag, sizeof(g_capture.tag), "key_%02u%02u_%02u%02u%02u_gi%d_gtao%d_cs%d_bk%d", t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, flags & 1, (flags >> 1) & 1, (flags >> 2) & 1, (flags >> 3) & 15);
        g_capture.byFrame = true;
        g_capture.count = keyFrames;
        g_capture.starts.assign(1, 0);
        g_capture.window = 0;
        m_dumpTag = g_capture.tag;
        m_giDumpWanted = true;
        m_engineCubeDumpWanted = true;
        m_sceneDumpIndex = 0;
        g_sceneDumpFrames = MvSequence() ? std::max(keyFrames - 1, 1) : 1; // the scenes before the capture's last present
    }
    if (g_capture.window < g_capture.starts.size() &&
        (g_capture.byFrame ? ::CryMTWorldFrame() : ProcessMilliseconds()) >= g_capture.starts[g_capture.window])
    {
        D3D12_RESOURCE_DESC rd = backbuffer->Desc12();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ID3D12Resource* buffer = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        if (buffer)
        {
            Require(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = buffer;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = backbuffer->res;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            NoteCopy("capture", dst, 0, 0, 0, src, nullptr);
            m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            g_capture.buffers.push_back(buffer);
            g_capture.footprints.push_back(fp);
        }
        g_capture.done++;
        if (g_capture.done == g_capture.count)
        {
            WaitIdle();
            for (size_t i = 0; i < g_capture.buffers.size(); i++)
            {
                CaptureWrite(static_cast<int>(i));
                g_capture.buffers[i]->Release();
            }
            Log("capture: window %zu, %zu frames written", g_capture.window + 1, g_capture.buffers.size());
            for (uint64_t k = m_frame >= 10 ? m_frame - 10 : 0; k < m_frame; k++)
            {
                const FrameNote& n = m_notes[k % 16];
                if (n.frame == k)
                    Log("capture: frame %llu: dlss %u gi %u, after DLSS: colour read %u, draws into it %u (with depth %u); copies of the "
                        "scene before DLSS %u, read after %u, given DLSS's picture %u; jitter told %+.3f %+.3f",
                        static_cast<unsigned long long>(k), n.dlss, n.gi, n.colourReads, n.lateDraws, n.late3D, n.copies, n.copyReads, n.fixed, n.jx, n.jy);
            }
            WriteDumps();
            g_capture.buffers.clear();
            g_capture.footprints.clear();
            g_capture.done = 0;
            g_capture.window++;
        }
    }
    bool cubeDump = false;
    if (g_cubeDumpWanted.exchange(false))
    {
        if (m_engineCube && m_dumps.empty())
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char tag[48];
            snprintf(tag, sizeof(tag), "cube_%02u%02u_%02u%02u%02u", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            m_dumpTag = tag;
            Require(m_engineCube->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            static const char* const faces[6] = {"envcube0", "envcube1", "envcube2", "envcube3", "envcube4", "envcube5"};
            for (UINT face = 0; face < 6; face++)
                RecordDump(m_engineCube->info->res, faces[face], 6, face);
            if (m_hdrCube.res) // the HDR copy the water reads, level 0: <tag>_hdrcube<face>.ppm (D3D layout, 1 - exp(-x))
            {
                Require(&m_hdrCube, D3D12_RESOURCE_STATE_COPY_SOURCE);
                FlushBarriers();
                static const char* const hdrFaces[6] = {"hdrcube0", "hdrcube1", "hdrcube2", "hdrcube3", "hdrcube4", "hdrcube5"};
                for (UINT face = 0; face < 6; face++)
                    RecordDump(m_hdrCube.res, hdrFaces[face], 0, face * m_hdrCube.mips);
                Require(&m_hdrCube, kSrvState);
                FlushBarriers();
            }
            cubeDump = true;
        }
        else
            Log("engine cube picture: no engine cube now (water reflection CUBE MAP or SSR + CUBE MAP with the engine's cube)");
    }
    Require(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_barriers.push_back(Transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
    FlushBarriers();
    m_list->CopyResource(target, backbuffer->res);
    m_barriers.push_back(Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
    Submit();
    const HRESULT hr = chain->Present(sync, flags);
    if (FAILED(hr) && !m_removedLogged)
    {
        Log("Present failed %08lx, device removed reason %08lx", static_cast<unsigned long>(hr), static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
        m_removedLogged = true;
        LogDred();
    }
    if (cubeDump)
    {
        WaitIdle();
        WriteDumps();
    }
    if (!m_voxSnap.empty())
    {
        WaitIdle();
        WriteVoxSnap();
    }
    m_frameFences[m_frame % kFramesInFlight] = g.signaled;
    m_frame++;
    m_stats.frames++;
    g_presented++;
    if (g_statsResetAsked.exchange(false))
    {
        m_stats = Stats{};
        std::fill(m_passTicks.begin(), m_passTicks.end(), 0);
        m_passStampsDropped = 0;
    }
    // Buffer memory over a long session (a heap that only grows ends in the engine's "Pipeline buffer overflow. Current
    // geometry cannot fit in video memory" when a page can no longer be created).
    if (m_frame % 3600 == 0)
    {
        // With the video memory the process uses against the budget Windows gives it (DXGI, local = the card's own).
        static IDXGIAdapter3* a3;
        if (!a3)
            g.factory->EnumAdapterByLuid(g.dev->GetAdapterLuid(), __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3));
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (a3)
        {
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
        }
        static uint64_t envBindsBefore, env2DBefore;
        static uint64_t ssrPrevBefore;
        static uint64_t partDrawsBefore;
        Log("environment map binds per frame: %.1f (2D maps %.1f); water SSR on the previous frame %.1f, particle layer draws %.1f",
            (g_device->m_envBinds - envBindsBefore) / 3600.0, (g_device->m_env2DBinds - env2DBefore) / 3600.0,
            (g_device->m_ssrPrevBinds - ssrPrevBefore) / 3600.0, (g_device->m_partDraws - partDrawsBefore) / 3600.0);
        ssrPrevBefore = g_device->m_ssrPrevBinds;
        partDrawsBefore = g_device->m_partDraws;
        envBindsBefore = g_device->m_envBinds;
        env2DBefore = g_device->m_env2DBinds;
        Log("heaps at frame %llu: video memory %.0f of %.0f MB, shared %.0f MB; upload pages %llu (+%llu dedicated), %.1f MB in free lists; "
            "default pages %llu (+%llu dedicated), %.1f MB in free lists",
            static_cast<unsigned long long>(m_frame), local.CurrentUsage / 1048576.0, local.Budget / 1048576.0, shared.CurrentUsage / 1048576.0,
            static_cast<unsigned long long>(g_slices.pages), static_cast<unsigned long long>(g_slices.dedicated), g_slices.FreeBytes() / 1048576.0,
            static_cast<unsigned long long>(g_blocks.pages), static_cast<unsigned long long>(g_blocks.dedicated), g_blocks.FreeBytes() / 1048576.0);
    }
    WaitFence(m_frameFences[m_frame % kFramesInFlight]);
    if (GetTickCount() - g_pipes.savedAt > 5000)
    {
        SavePipelines();
        g_pipes.savedAt = GetTickCount();
    }
    return hr;
}

constexpr UINT kSwapBuffers = 3;
std::atomic<uint32_t> g_screenWidth, g_screenHeight; // the back buffer, for the camera jitter

DXGI_FORMAT SwapFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return f;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

class SwapChain12 final : public IDXGISwapChain
{
public:
    IDXGISwapChain3* chain = nullptr;
    DXGI_SWAP_CHAIN_DESC desc{}; // as the renderer asked, with the current size
    UINT flags = 0;              // the real swap chain's flags
    bool windowed = true;
    bool borderless = false;     // fullscreen requests go to the borderless window (Window.h); the chain stays windowed
    ID3D12Resource* buffers[kSwapBuffers] = {};
    Texture2D12* backbuffer = nullptr;

    bool CreateBuffers()
    {
        DXGI_SWAP_CHAIN_DESC1 d1{};
        chain->GetDesc1(&d1);
        desc.BufferDesc.Width = d1.Width;
        desc.BufferDesc.Height = d1.Height;
        g_screenWidth = d1.Width;
        g_screenHeight = d1.Height;
        for (UINT i = 0; i < kSwapBuffers; i++)
            if (FAILED(chain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffers[i]))))
                return false;
        D3D10_TEXTURE2D_DESC td{};
        td.Width = d1.Width;
        td.Height = d1.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = desc.BufferDesc.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D10_USAGE_DEFAULT;
        td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
        ID3D10Texture2D* tex = nullptr;
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &tex)))
            return false;
        backbuffer = static_cast<Texture2D12*>(tex);
        return true;
    }

    void ReleaseBuffers()
    {
        for (ID3D12Resource*& b : buffers)
            if (b)
            {
                b->Release();
                b = nullptr;
            }
        if (backbuffer)
        {
            backbuffer->Release();
            backbuffer = nullptr;
        }
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDXGIObject)) || IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain)))
        {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        LogGuid("swap chain", riid);
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
        {
            g_device->WaitIdle();
            ReleaseBuffers();
            if (!windowed)
                chain->SetFullscreenState(FALSE, nullptr);
            chain->Release();
            PrivateErase(this);
            delete this;
        }
        return refs;
    }

    // ---- IDXGIObject, IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        return PrivateSet(this, guid, size, data, nullptr);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override
    {
        return PrivateSet(this, guid, 0, nullptr, object);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        return PrivateGet(this, guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override
    {
        return chain->GetParent(riid, parent);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override
    {
        return g_device->QueryInterface(riid, device);
    }

    // ---- IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT presentFlags) override
    {
        if (presentFlags & DXGI_PRESENT_TEST)
            return chain->Present(sync, presentFlags);
        if (windowed && sync == 0 && (flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
            presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
        return g_device->PresentFrame(chain, buffers[chain->GetCurrentBackBufferIndex()], backbuffer, sync, presentFlags);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID riid, void** surface) override
    {
        return backbuffer ? backbuffer->QueryInterface(riid, surface) : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(WINBOOL fullscreen, IDXGIOutput* target) override
    {
        if (borderless)
        {
            BorderlessSetFullscreen(desc.OutputWindow, fullscreen != FALSE);
            Log("SetFullscreenState(%d): borderless", fullscreen);
            return S_OK;
        }
        g_device->WaitIdle();
        const HRESULT hr = chain->SetFullscreenState(fullscreen, target);
        if (SUCCEEDED(hr))
            windowed = !fullscreen;
        Log("SetFullscreenState(%d) %08lx", fullscreen, static_cast<unsigned long>(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(WINBOOL* fullscreen, IDXGIOutput** target) override
    {
        if (!borderless)
            return chain->GetFullscreenState(fullscreen, target);
        if (fullscreen)
            *fullscreen = BorderlessFullscreen();
        if (target)
            *target = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override
    {
        if (!d)
            return E_INVALIDARG;
        *d = desc;
        d->Windowed = borderless ? !BorderlessFullscreen() : windowed;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT width, UINT height, DXGI_FORMAT format, UINT) override
    {
        g_device->WaitIdle();
        ReleaseBuffers();
        if (format != DXGI_FORMAT_UNKNOWN)
            desc.BufferDesc.Format = format;
        const HRESULT hr = chain->ResizeBuffers(kSwapBuffers, width, height, format != DXGI_FORMAT_UNKNOWN ? SwapFormat(format) : DXGI_FORMAT_UNKNOWN, flags);
        if (!CreateBuffers())
            Log("ResizeBuffers: buffers not recreated");
        Log("ResizeBuffers %ux%u fmt %d: %08lx, now %ux%u", width, height, format, static_cast<unsigned long>(hr), desc.BufferDesc.Width,
            desc.BufferDesc.Height);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override
    {
        return borderless && BorderlessFullscreen() ? S_OK : chain->ResizeTarget(mode);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override
    {
        return chain->GetContainingOutput(output);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override
    {
        return chain->GetFrameStatistics(stats);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override
    {
        return chain->GetLastPresentCount(count);
    }

private:
    std::atomic<ULONG> m_refs{1};
};

// ---------------------------------------------------------------------------------------------------------------------
// Start-up.
// ---------------------------------------------------------------------------------------------------------------------
bool InitCore(IDXGIAdapter* engineAdapter)
{
    rdoc::Load(); // before the device and the factory, so RenderDoc sees them
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d12 || !dxgi)
    {
        Log("d3d12.dll or dxgi.dll not found");
        return false;
    }
    const auto create = reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    const auto getDebug = reinterpret_cast<GetDebugInterfaceFn>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
    const auto serialize = reinterpret_cast<SerializeRootSignatureFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
    g_serializeRoot = serialize;
    const auto createFactory = reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
    if (!create || !serialize || !createFactory)
    {
        Log("D3D12 or DXGI 1.3 entry points missing");
        return false;
    }
    const bool debug = GetEnvironmentVariableW(L"CRYMT_D3D12DEBUG", nullptr, 0) != 0;
    if (debug && getDebug)
    {
        ID3D12Debug* d = nullptr;
        if (SUCCEEDED(getDebug(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&d))))
        {
            d->EnableDebugLayer();
            d->Release();
            Log("debug layer enabled");
        }
    }
    // DRED (device removed extended data): after a GPU hang or page fault LogDred writes the unfinished command lists' last
    // operations and the faulting address with the allocations there. Page faults always; auto-breadcrumbs unless
    // CRYMT_DRED=0 (the user's GPU hang of 2026-11-12 left only "device removed 887a0006" behind).
    if (getDebug)
    {
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        if (SUCCEEDED(getDebug(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), reinterpret_cast<void**>(&dred))))
        {
            wchar_t v[8] = {};
            const bool crumbs = !(GetEnvironmentVariableW(L"CRYMT_DRED", v, 8) && wcscmp(v, L"0") == 0);
            dred->SetAutoBreadcrumbsEnablement(crumbs ? D3D12_DRED_ENABLEMENT_FORCED_ON : D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
            Log("DRED: page faults on, breadcrumbs %s", crumbs ? "on" : "off");
        }
    }
    if (FAILED(createFactory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&g.factory))))
    {
        Log("CreateDXGIFactory2 failed");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    if (engineAdapter)
    {
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(engineAdapter->GetDesc(&ad)))
        {
            g.factory->EnumAdapterByLuid(ad.AdapterLuid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
            Log("adapter: %ls", ad.Description);
        }
    }
    const HRESULT hr = create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g.dev));
    if (adapter)
        adapter->Release();
    if (FAILED(hr))
    {
        Log("D3D12CreateDevice failed %08lx", static_cast<unsigned long>(hr));
        return false;
    }
    wchar_t dlssVar[8] = {};
    if (GetEnvironmentVariableW(L"CRYMT_DLSS", dlssVar, 8) != 0 && wcscmp(dlssVar, L"0") != 0 && rdoc::Loaded())
        Log("DLSS off: RenderDoc is in the process");
    else if (GetEnvironmentVariableW(L"CRYMT_DLSS", dlssVar, 8) != 0 && wcscmp(dlssVar, L"0") != 0) // 0: off
        dlss::Init(g.dev, g.dev->GetAdapterLuid(), [](const char* text) { Log("%s", text); });
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (FAILED(dlss::CreateQueue(g.dev, &qd, &g.queue)) ||
        FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g.fence))))
    {
        Log("queue or fence creation failed");
        return false;
    }
    g.srvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.rtvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g.dsvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    g.samplerInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    g_srvCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    g_srvCpu.inc = g.srvInc;
    g_rtvCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_rtvCpu.inc = g.rtvInc;
    g_dsvCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    g_dsvCpu.inc = g.dsvInc;
    g_samplerCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    g_samplerCpu.inc = g.samplerInc;
    if (!CreateRootSignature(serialize))
        return false;
    g_nullSrv = g_srvCpu.Alloc();
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    g.dev->CreateShaderResourceView(nullptr, &nullSrv, g_nullSrv);
    g_defaultSampler = g_samplerCpu.Alloc();
    D3D12_SAMPLER_DESC sd{};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    g.dev->CreateSampler(&sd, g_defaultSampler);
    if (debug && SUCCEEDED(g.dev->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&g.info))))
    {
        // Clears without an optimized clear value (the renderer's clear colors vary): expected, not an error.
        // Depth-only passes keep a pixel shader that writes a color target: D3D10 discards the writes too.
        D3D12_MESSAGE_ID deny[] = {static_cast<D3D12_MESSAGE_ID>(820), static_cast<D3D12_MESSAGE_ID>(821), static_cast<D3D12_MESSAGE_ID>(679)};
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = 3;
        filter.DenyList.pIDList = deny;
        g.info->AddStorageFilterEntries(&filter);
    }
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(g.factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5))))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            g.tearing = allow != FALSE;
        f5->Release();
    }
    g.reflect = reinterpret_cast<ReflectShaderFn>(GetProcAddress(GetModuleHandleW(L"d3d10.dll"), "D3D10ReflectShader"));
    g_initRing.Init(128u << 20);
    if (!g_initRing.res)
        return false;
    OpenPipelines();
    CaptureInit();
    CaptureStreamInit();
    Log("D3D12 device ready: tearing %d, reflection %d", g.tearing, g.reflect != nullptr);
    return true;
}
} // namespace
} // namespace dx12

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
