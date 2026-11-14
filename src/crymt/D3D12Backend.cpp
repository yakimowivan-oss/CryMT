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

// The depth pyramid for screen-space reflections (RunHiz): level 0 = near / view depth from the z-prepass's linear depth
// (w / far), 0 for the sky and the first-person view (the hardware depth under 0.1: rays pass it); each further level the
// largest of 2x2 (inverted depth: the nearest). Constants: x, y the level's size, z, w the source level's size, near / far,
// 1 with the hardware depth.
const char* const kHizShader = R"(
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
)";

// The engine's environment cube as the back end keeps it (Device::RunCubeUpdate): HDR, in D3D's cube layout, with all its
// mip levels. cubeDecode: one face from the engine's RGBK cube (rgb a 16), flipped vertically (the engine's face images
// are), mixed into the old content by `blend` (1 replaces); cubeDown: the next level of all six faces, each texel the
// level above looked up as a cube by the texel's direction (bilinear at the corner of 2x2: their mean; across the faces'
// edges the hardware's cube filtering takes the neighbour's texels, so the faces meet at every level: per-face 2x2 means
// left seams on far water). Constants: the written level's size, the face, the blend.
const char* const kCubeShader = R"(
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
)";
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
const char* const kDepthAlignShader = R"(
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
)";

const char* const kGiShader = R"(
cbuffer GiConstants : register(b0)
{
    float2 fullSize; float2 giSize;
    float2 invProj;     // 1 / proj._11, 1 / proj._22
    float2 depthParams; // proj._33, proj._43 (right-handed): distance = _43 / (d + _33)
    float radius; float bounce; float occlusion; float temporal;
    float maxDistance; float frame; float debug; float options; // options: 1 fixed sample pattern, 2 nearest depth of the 2x2 block, 4 plain bounce sum,
                                                                 // 8 narrow normals, 16 the first-person view takes the camera's GI (upsample t2),
                                                                 // 32 two screen-part directions (else four), 64 opacity per axis,
                                                                 // 128 the roof texture (CRYMT_GI_ROOFCELLS), 256 thin features
                                                                 // take their surroundings' GI (CRYMT_GI_THIN, on); upsample:
                                                                 // 512 the GI replaces the engine's SSAO (GTAO), 1024 GTAO's
                                                                 // own history, 2048-16384 the baked terms replaced, 32768
                                                                 // GTAO at full size, 65536 x half TerrainAO's GI distance
                                                                 // (5 bits), 2^21 the blended particles' GI,
                                                                 // 2^22 foliage occludes nothing in GTAO, 2^23 GTAO on
                                                                 // foliage in the sun's shadow at 0.2 (all exact in a float)
    row_major float4x4 reproj; // this frame's view space (x, y, -distance) to the previous frame's clip space
    float4 motionParams;       // motion: x far plane, y 1 with the engine's object velocities in t1; GI: z the ambient
                               // floor (Strengthen)
    float4 toWorldX; float4 toWorldY; float4 toWorldZ; // the engine's view (x, y, -distance, 1) dotted with these: world
    float4 voxGrid;   // xyz the grid's first cell, w cells per axis
    float4 voxParams; // x cell size (m), y 0 off / 1 on / 2 voxel part only / 3 screen part only, z cone reach (m), w strength
    float4 voxParams2; // x occlusion strength of the cones; near occlusion cones: y lift, z start (cells of level 0),
                       // w strength (0: none)
    float2 occRange;   // x: the cones' occlusion counts fully up to x / 2 metres, fading to none at x (light from all the
                       // reach); y: the occlusion strength where there is cover above (Strengthen)
};
// trace: t0 depth, t1 colour, u0 raw. filter: t0 depth, t1 raw, t2 previous GI, u0 new GI, u1 distance out, u2 previous.
Texture2D<float> depthTex : register(t0);
Texture2D<float4> srcTex : register(t1);
Texture2D<float4> historyTex : register(t2);
RWTexture2D<float4> outTex : register(u0);
RWTexture2D<float> distOut : register(u1);
RWTexture2D<float> distPrev : register(u2);
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);
Texture3D<float4> voxelTex : register(t3);  // grid levels 0..3: cells of voxParams.x, twice, 4 and 8 times that
Texture3D<float4> voxelTex1 : register(t4);
Texture3D<float4> voxelTex2 : register(t5);
Texture3D<float4> voxelTex3 : register(t6);
Texture3D<float4> dirTex : register(t7);   // the levels' opacity per axis (xyz) at mips 1..4 of the grid (options & 64)
Texture3D<float4> dirTex1 : register(t8);
Texture3D<float4> dirTex2 : register(t9);
Texture3D<float4> dirTex3 : register(t10);
Buffer<float4> visAreas : register(t11); // [0].x count; per area 10: x, y box, (z min, z max, points, 0), 16 points (x, y pairs)
// After the 16 areas, [161]: GI v2's options, x 1 the precise rays (CRYMT_GI_PROBEPRECISE, the panel's "Probe rays").
static const int kProbeOptions = 161;
Texture3D<float> roofTex : register(t12); // cover above per cell of level 0 at half its resolution (RoofAt)
Texture3D<float4> l1Tex : register(t13);  // the levels' light direction g (CRYMT_GI_DIRLIGHT 1, visAreas[0].z): out towards w
Texture3D<float4> l1Tex1 : register(t14); // the colour times 1 + g.w
Texture3D<float4> l1Tex2 : register(t15);
Texture3D<float4> l1Tex3 : register(t16);
// The levels' six directions (CRYMT_GI_DIRLIGHT 2, visAreas[0].z 2): t17 + level x 6 + face (+X, -X, +Y, -Y, +Z, -Z).
Texture3D<float3> sixTex[24] : register(t17);
// GI v2 (ProbeLight, pass "probes"): the probes' atlases written this frame: light and states (t41), distances (t42).
// The probes pass writes the distances through u1 (the trace's distOut there is another entry's) and reads last frame's
// through t1.
Texture2D<float4> probeTex : register(t41);
Texture2D<float4> probeDepthTex : register(t42);
RWTexture2D<float4> probeDepthOut : register(u1);

// The VisArea near the camera that holds P (the rooms the level marks as indoors; with a margin of 0.3 m, so walls on
// their border count), else -1.
int VisAreaAt(float3 P)
{
    int count = (int)visAreas[0].x;
    [loop] for (int k = 0; k < count; k++)
    {
        int b = 1 + k * 10;
        float4 box = visAreas[b], zr = visAreas[b + 1];
        if (any(P.xy < box.xy - 0.3) || any(P.xy > box.zw + 0.3) || P.z < zr.x - 0.3 || P.z > zr.y + 0.3)
            continue;
        int n = (int)zr.z;
        bool odd = false;
        float edge = 1e9;
        [loop] for (int i = 0; i < n; i++)
        {
            int j = i + 1 == n ? 0 : i + 1;
            float4 pa = visAreas[b + 2 + i / 2], pc = visAreas[b + 2 + j / 2];
            float2 a = (i & 1) ? pa.zw : pa.xy, c = (j & 1) ? pc.zw : pc.xy;
            if ((a.y > P.y) != (c.y > P.y) && P.x < a.x + (P.y - a.y) * (c.x - a.x) / (c.y - a.y))
                odd = !odd;
            float2 e = c - a, w = P.xy - a;
            edge = min(edge, length(w - e * saturate(dot(w, e) / max(dot(e, e), 1e-6))));
        }
        if (odd || edge < 0.3)
            return k;
    }
    return -1;
}

// 1 inside one of the VisAreas near the camera, else 0.
float VisInside(float3 P) { return VisAreaAt(P) >= 0 ? 1 : 0; }

// The room the GI's cones start in (CRYMT_GI_ROOMLIGHT, visAreas[0].y): its VisArea's box (with the 0.3 m margin);
// samples outside it bring no light, they still block. Sunlit roofs and outer walls came in through the coarse mips of
// thin walls (the user, village 2026-11-13: "the sun outside lights the walls inside, they are overexposed"; the GI
// darkened the stair 52.8 -> 27.2 but its upper walls and ceiling stayed light). g_roomZ.x > g_roomZ.y: no room.
static float4 g_roomXY = 0;
static float2 g_roomZ = float2(1, 0);
void SetRoom(int k)
{
    int b = 1 + k * 10;
    g_roomXY = visAreas[b] + float4(-0.3, -0.3, 0.3, 0.3);
    g_roomZ = visAreas[b + 1].xy + float2(-0.3, 0.3);
}
// How much of a sample's footprint (radius r) lies inside the room's box: 1 wholly inside, 0.5 centred on its wall,
// 0 wholly outside; 1 without a room.
float RoomWeight(float3 X, float r)
{
    if (g_roomZ.x > g_roomZ.y)
        return 1;
    float3 inside = min(X - float3(g_roomXY.xy, g_roomZ.x), float3(g_roomXY.zw, g_roomZ.y) - X);
    return saturate(min(inside.x, min(inside.y, inside.z)) / max(2 * r, 0.25) + 0.5);
}
SamplerState linearWrap : register(s2);

static const float kBias = 0.15; // horizon elevation ignored: depth steps and normal noise (no false occlusion or light)
// The first-person weapon is drawn with its own camera into depth 0..0.1 (viewport); the world lies behind. No GI on
// it, and it is no occluder for the world.
static const float kWeaponDepth = 0.1;

float Distance(float2 uv) { return depthParams.y / (depthTex.SampleLevel(pointClamp, uv, 0) + depthParams.x); }

float3 ViewPosAt(float2 uv, float d)
{
    float z = depthParams.y / (d + depthParams.x);
    return float3(float2(uv.x * 2 - 1, 1 - uv.y * 2) * invProj * z, z);
}

float3 ViewPos(float2 uv) { return ViewPosAt(uv, depthTex.SampleLevel(pointClamp, uv, 0)); }

// The scene colour can hold NaN or infinity, and min/max turn those into the clamp value (white dots); once in the
// history they never leave (0 * NaN). Integer test: the compiler may fold float NaN tests away.
bool Finite(float4 v) { return all((asuint(v) & 0x7f800000) != 0x7f800000); }

// Depth texel centre: positions rebuilt between texels are off the surface by up to half a texel, which on slopes
// turns into moire bands of false occlusion.
float2 Snap(float2 uv) { return (floor(uv * fullSize) + 0.5) / fullSize; }

// A cone through the voxel grid from world position O along dir (half angle tangent halfTan), from distance start:
// front-to-back over the grid's mips (premultiplied colour), rgb light arriving, a share blocked. Stops at the grid's edge.
// Inside grid level l with a margin of three cells. Level 0's first cell is its last update's; the coarser levels are
// updated every 2nd, 4th, 8th frame and their first cells are taken from the camera (floor(eye / cell) - n / 2), which
// may run up to a couple of cells ahead of their contents: the margin keeps the cones off cells not yet reset.
bool InGrid(int level, float3 X, float radius)
{
    float vs = voxParams.x * (1 << level), n = voxGrid.w;
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float3 g = level == 0 ? voxGrid.xyz : floor(eye / vs) - n / 2;
    return all(X - radius > (g + 3) * vs) && all(X + radius < (g + n - 3) * vs);
}

// How many cells a footprint lies inside level l's usable region (InGrid's): over the last 8 the cones go over to the
// next level (dithered per sample, one sample as before; the temporal filter smooths it), so a level's edge shows no seam
// in the GI as the camera moves (a blend of both levels cost 0.5 ms a frame on the demo).
float EdgeCells(int level, float3 X, float radius)
{
    float vs = voxParams.x * (1 << level), n = voxGrid.w;
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float3 g = level == 0 ? voxGrid.xyz : floor(eye / vs) - n / 2;
    float3 lo = (X - radius) / vs - (g + 3), hi = (g + n - 3) - (X + radius) / vs;
    return min(min(lo.x, min(lo.y, lo.z)), min(hi.x, min(hi.y, hi.z)));
}

// The opacity per axis at grid mip (1..4): the per-axis texture's mip one lower.
float3 SampleDir(int level, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    if (level == 0)
        return dirTex.SampleLevel(linearWrap, uvw, mip - 1).xyz;
    if (level == 1)
        return dirTex1.SampleLevel(linearWrap, uvw, mip - 1).xyz;
    if (level == 2)
        return dirTex2.SampleLevel(linearWrap, uvw, mip - 1).xyz;
    return dirTex3.SampleLevel(linearWrap, uvw, mip - 1).xyz;
}

float3 SampleSix(int level, int face, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    [forcecase] switch (level * 6 + face)
    {
    case 0: return sixTex[0].SampleLevel(linearWrap, uvw, mip);
    case 1: return sixTex[1].SampleLevel(linearWrap, uvw, mip);
    case 2: return sixTex[2].SampleLevel(linearWrap, uvw, mip);
    case 3: return sixTex[3].SampleLevel(linearWrap, uvw, mip);
    case 4: return sixTex[4].SampleLevel(linearWrap, uvw, mip);
    case 5: return sixTex[5].SampleLevel(linearWrap, uvw, mip);
    case 6: return sixTex[6].SampleLevel(linearWrap, uvw, mip);
    case 7: return sixTex[7].SampleLevel(linearWrap, uvw, mip);
    case 8: return sixTex[8].SampleLevel(linearWrap, uvw, mip);
    case 9: return sixTex[9].SampleLevel(linearWrap, uvw, mip);
    case 10: return sixTex[10].SampleLevel(linearWrap, uvw, mip);
    case 11: return sixTex[11].SampleLevel(linearWrap, uvw, mip);
    case 12: return sixTex[12].SampleLevel(linearWrap, uvw, mip);
    case 13: return sixTex[13].SampleLevel(linearWrap, uvw, mip);
    case 14: return sixTex[14].SampleLevel(linearWrap, uvw, mip);
    case 15: return sixTex[15].SampleLevel(linearWrap, uvw, mip);
    case 16: return sixTex[16].SampleLevel(linearWrap, uvw, mip);
    case 17: return sixTex[17].SampleLevel(linearWrap, uvw, mip);
    case 18: return sixTex[18].SampleLevel(linearWrap, uvw, mip);
    case 19: return sixTex[19].SampleLevel(linearWrap, uvw, mip);
    case 20: return sixTex[20].SampleLevel(linearWrap, uvw, mip);
    case 21: return sixTex[21].SampleLevel(linearWrap, uvw, mip);
    case 22: return sixTex[22].SampleLevel(linearWrap, uvw, mip);
    default: return sixTex[23].SampleLevel(linearWrap, uvw, mip);
    }
}

// The light the cells send back along a cone going along dir (out towards -dir): the three faces turned to it, by dir^2.
float3 SixToward(int level, float3 X, float mip, float3 dir)
{
    float3 w2 = dir * dir;
    return w2.x * SampleSix(level, dir.x > 0 ? 1 : 0, X, mip) + w2.y * SampleSix(level, dir.y > 0 ? 3 : 2, X, mip) +
           w2.z * SampleSix(level, dir.z > 0 ? 5 : 4, X, mip);
}

float3 SampleL1(int level, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    if (level == 0)
        return l1Tex.SampleLevel(linearWrap, uvw, mip).xyz;
    if (level == 1)
        return l1Tex1.SampleLevel(linearWrap, uvw, mip).xyz;
    if (level == 2)
        return l1Tex2.SampleLevel(linearWrap, uvw, mip).xyz;
    return l1Tex3.SampleLevel(linearWrap, uvw, mip).xyz;
}

float4 SampleGrid(int level, float3 X, float mip)
{
    float3 uvw = X / (voxParams.x * (1 << level) * voxGrid.w);
    if (level == 0)
        return voxelTex.SampleLevel(linearWrap, uvw, mip);
    if (level == 1)
        return voxelTex1.SampleLevel(linearWrap, uvw, mip);
    if (level == 2)
        return voxelTex2.SampleLevel(linearWrap, uvw, mip);
    return voxelTex3.SampleLevel(linearWrap, uvw, mip);
}

// The cell X lies in at mip 0, no filtering (the probes' thin rays): a trilinear sample took the neighbouring cells, so
// a probe a few centimetres over sunlit sand saw the sand in every direction, up too, and the ground lit itself through
// the probes (the user, 2026-11-14: "outside everything is overexposed"; the bounce on island's beach 3.3 x the cones').
int4 GridCell(int level, float3 X)
{
    const int n = (int)voxGrid.w;
    const int3 c = (int3)floor(X / (voxParams.x * (1 << level)));
    return int4(c & (n - 1), 0); // n = 128: a mask wraps negative cells too
}

float4 LoadGrid(int level, float3 X)
{
    const int4 c = GridCell(level, X);
    if (level == 0)
        return voxelTex.Load(c);
    if (level == 1)
        return voxelTex1.Load(c);
    if (level == 2)
        return voxelTex2.Load(c);
    return voxelTex3.Load(c);
}

float3 LoadL1(int level, float3 X)
{
    const int4 c = GridCell(level, X);
    if (level == 0)
        return l1Tex.Load(c).xyz;
    if (level == 1)
        return l1Tex1.Load(c).xyz;
    if (level == 2)
        return l1Tex2.Load(c).xyz;
    return l1Tex3.Load(c).xyz;
}

// The cone samples the finest grid level that holds it and has a mip as coarse as its diameter (mip m of level l has
// cells of voxParams.x * 2^(l + m)); past the outer level's edge it stops. A sample whose footprint still reaches the
// plane of the surface the cone starts from (Ps, Ns) counts only for the part above it: near starts would otherwise take
// the surface's own cells as light and occlusion (open sand got brighter).
float4 ConeTrace(float3 O, float3 dir, float halfTan, float start, float reach, bool plane, bool surface, float3 Ps, float3 Ns, float seed)
{
    float vs = voxParams.x;
    float3 light = 0;
    float blocked = 0, occ = 0, d = start; // blocked: what the light behind passes; occ: the occlusion handed back
    // The gap before the first sample, through level 0's cells (mip 0, half a cell a step): a wall nearer than the
    // start stops the cone there. Else a cone from a ceiling or floor next to a wall started behind the wall and
    // brought the light and openness of the other side (bright bands along the edges of rooms). Not for the near
    // cones (they start within a cell of their surface).
    if (start > vs * 1.5 && InGrid(0, O, start))
        [loop] for (float t = vs * 0.5; t < start && blocked < 0.95; t += vs * 0.5)
        {
            float a = SampleGrid(0, O + dir * t, 0).a;
            occ += (1 - blocked) * a;
            blocked += (1 - blocked) * a;
        }
    [loop] for (int i = 0; i < 32 && blocked < 0.95 && d < reach; i++)
    {
        float diameter = max(2 * d * halfTan, 2 * vs);
        float3 X = O + dir * d;
        float lod = log2(diameter / vs);
        int level = 0;
        [loop] while (level < 3 && (lod - level > 4 || !InGrid(level, X, diameter * 0.5)))
            level++;
        if (!InGrid(level, X, diameter * 0.5))
            break;
        // Within 8 cells of the level's edge the next level is blended in (it takes over at the edge).
        float edge = level < 3 && InGrid(level + 1, X, diameter * 0.5) ? saturate(EdgeCells(level, X, diameter * 0.5) / 8) : 1;
        float mip = clamp(lod - level, 1, 4);
        float4 v = SampleGrid(level, X, mip);
        // Opacity per axis (CRYMT_GI_MIPOPACITY=4): a wall stops what crosses it. From a surface (Ps, Ns) the axis along
        // its normal counts only once the sample is clear of it by half its size: a coarse cell around a cone from the
        // ground holds the ground, opaque across, which darkened open ground; the far wall or the ceiling count fully.
        float3 axes = surface ? lerp(1, saturate(2 * dot(X - Ps, Ns) / diameter - 1), Ns * Ns) : 1;
        if (((uint)options & 64) != 0)
            v.a = dot(dir * dir * axes, SampleDir(level, X, mip));
        if (edge < 1)
        {
            float mip1 = clamp(lod - level - 1, 1, 4);
            float4 w = SampleGrid(level + 1, X, mip1);
            if (((uint)options & 64) != 0)
                w.a = dot(dir * dir * axes, SampleDir(level + 1, X, mip1));
            v = lerp(w, v, edge);
        }
        // CRYMT_GI_DIRLIGHT 2: the six directions' faces turned to the cone; 1: colour x (1 - g.dir) (out towards -dir).
        if (visAreas[0].z > 1.5)
        {
            float3 c6 = SixToward(level, X, mip, dir);
            if (edge < 1)
                c6 = lerp(SixToward(level + 1, X, clamp(lod - level - 1, 1, 4), dir), c6, edge);
            v.rgb = c6;
        }
        else if (visAreas[0].z > 0.5)
        {
            float3 gl = SampleL1(level, X, mip);
            if (edge < 1)
                gl = lerp(SampleL1(level + 1, X, clamp(lod - level - 1, 1, 4)), gl, edge);
            v.rgb *= max(1 - dot(gl, dir), 0);
        }
        float above = plane ? saturate((dot(X - Ps, Ns) - diameter * 0.5) / (diameter * 0.5)) : 1;
        light += (1 - blocked) * v.rgb * above * RoomWeight(X, diameter * 0.5);
        // Distant cover (hills, forest tens of metres away) took the ambient of open ground: occlusion counts fully up
        // to occRange.x, fading out by occRange.y (Crytek's cone max length is 12 m); indoors the cover is near.
        float near = occRange.x > 0 ? saturate((occRange.x - d) / (occRange.x * 0.5)) : 1;
        occ += (1 - blocked) * v.a * above * near;
        blocked += (1 - blocked) * v.a * above;
        d += diameter * 0.6;
    }
    return float4(light, occ);
}

// Cover above a point (0 open sky .. 1 a roof) from the roof texture (kVoxShader "roof": level 0 at half its
// resolution, the share of five rays up that real geometry stops within 8 m; foliage is no roof); 0 outside level 0.
float RoofAt(float3 X)
{
    return InGrid(0, X, 0) ? roofTex.SampleLevel(linearWrap, X / (voxParams.x * voxGrid.w), 0) : 0;
}

// The same along one ray from the texel (up to 8 m: occRange.x / 2, through the finest level that holds it, a cell a
// step from a jittered start, mip 0): finds the ceiling over a doorway, where the cells half a metre off a wall did
// not (village 2250: 59 -> 48% darkening with RoofAt alone).
float RoofCover(float3 O, float3 dir, float seed)
{
    float covered = 0, d = voxParams.x * (1 + seed), reach = occRange.x > 0 ? occRange.x * 0.5 : 8;
    [loop] for (int i = 0; i < 40 && covered < 0.95 && d < reach; i++)
    {
        float3 X = O + dir * d;
        int level = 0;
        [loop] while (level < 3 && !InGrid(level, X, 0))
            level++;
        if (!InGrid(level, X, 0))
            break;
        covered += (1 - covered) * SampleGrid(level, X, 0).a;
        d += voxParams.x * (1 << level);
    }
    return covered;
}

// The occlusion strength: indoors (a VisArea, else cover above: RoofAt) Num4 / Num7 (CRYMT_GI_OCCSTRENGTH,
// occRange.y), outdoors CRYMT_GI_OUTDOOR (motionParams.w), blended by the indoor share. A plain factor of 2 that suited rooms darkened everything
// outdoors; by a point's own occlusion (a curve) it could not part the two, a bush a metre away occludes as much as a
// wall (2.0 / 3.0: hut 31.7 / 26.0, path 57.7 / 56.4 vs the plain factor 23.9 / 56.0).
// Then the ambient floor (CRYMT_GI_FLOOR, motionParams.z): occ (1 - floor occ) keeps that share of the flat ambient where
// the occlusion is full and leaves light occlusion (open ground) almost as it is - with opaque coarse cells
// (CRYMT_GI_MIPOPACITY) rooms otherwise lose all of it (Crytek's TI has a diffuse bias for the same).
float Strengthen(float occ, float indoor)
{
    float o = saturate(saturate(occ) * lerp(motionParams.w, occRange.y, indoor));
    return o * (1 - motionParams.z * o);
}

// The depth texel a GI texel stands for: the top-left one of its 2x2 block, or (options & 2) the nearest one that is not
// the weapon. With DLAA jitter a fixed texel lands on thin geometry in one frame and on what lies behind in the next, so
// the GI texel flips and loses its history; the nearest of four keeps thin geometry its own texel, and the full-size
// pass gives what lies behind the GI of its neighbours.

// Full-size pixels per GI texel each way (2 or 4: Device::GiScale).
int GiScale() { return (int)round(fullSize.x / giSize.x); }

float2 GiDepthUv(int2 gi)
{
    int f = GiScale();
    int2 p = min(gi * f, int2(fullSize) - 1);
    if ((uint)options & 2)
    {
        float best = 2;
        int2 at = p;
        [unroll] for (int j = 0; j < 4; j++)
        {
            int2 q = min(gi * f + int2(j & 1, j >> 1) * (f / 2), int2(fullSize) - 1);
            float d = depthTex.Load(int3(q, 0));
            if (d >= kWeaponDepth && d < best)
            {
                best = d;
                at = q;
            }
        }
        p = at;
    }
    return (p + 0.5) / fullSize;
}

// Environment maps from the voxel grid (CRYMT_ENVMAP=voxels), for the materials' envMapSamplerCUBE / envMapSampler and
// fragLib's CryMT_EnvSampler. u0 an atlas of 26 squares of kEnvSize (13..25: what the materials get, below), t2 the
// previous run's atlas: 0..5 the cube's faces
// +X -X +Y -Y +Z -Z (D3D order, world axes: materials look it up by the world reflection vector), 6 the 2D map the
// weapons' Metal reads by reflection.xy (wrapped: a texel stands for xy and xy - 1, both upward), 7..12 the sky per
// direction, kept between runs: where the previous frame shows the far plane in a direction it moves toward that colour,
// elsewhere it stays (a = 1 seen, 0.5 only the screen's average so far). Per direction a cone from the camera through
// the grid; what it leaves unblocked sees that sky. The result is blended with the previous run's (no jumps when the
// grid or the view changes) and brought to 0..1 as the game's own environment maps are (the materials multiply it by
// the light at the surface).
static const uint kEnvSize = 64;
static const float kEnvExposure = 0.5;
groupshared float4 envSky[64];

float3 EnvFaceDir(uint face, float2 f)
{
    float u = f.x * 2 - 1, w = f.y * 2 - 1;
    return face == 0 ? float3(1, -w, -u) : face == 1 ? float3(-1, -w, u) : face == 2 ? float3(u, 1, w) :
           face == 3 ? float3(u, -1, -w) : face == 4 ? float3(u, -w, 1) : float3(-u, -w, -1);
}

// The texel of square `first` + face for a direction (the inverse of EnvFaceDir).
int2 EnvTexel(float3 d, uint first)
{
    float3 a = abs(d);
    uint face;
    float2 uv;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x > 0 ? 0 : 1;
        uv = float2(d.x > 0 ? -d.z : d.z, -d.y) / a.x;
    }
    else if (a.y >= a.z)
    {
        face = d.y > 0 ? 2 : 3;
        uv = float2(d.x, d.y > 0 ? d.z : -d.z) / a.y;
    }
    else
    {
        face = d.z > 0 ? 4 : 5;
        uv = float2(d.z > 0 ? d.x : -d.x, -d.y) / a.z;
    }
    int2 t = clamp((int2)((uv * 0.5 + 0.5) * kEnvSize), 0, kEnvSize - 1);
    return int2((first + face) * kEnvSize + t.x, t.y);
}

float4 EnvTrace(float3 O, float3 dir, float halfTan)
{
    float vs = voxParams.x;
    float3 light = 0;
    float blocked = 0, d = vs;
    [loop] for (int i = 0; i < 48 && blocked < 0.97; i++)
    {
        float diameter = max(2 * d * halfTan, vs);
        float3 X = O + dir * d;
        float lod = log2(diameter / vs);
        int level = 0;
        [loop] while (level < 3 && (lod - level > 4 || !InGrid(level, X, diameter * 0.5)))
            level++;
        if (!InGrid(level, X, diameter * 0.5))
            break;
        float4 v = SampleGrid(level, X, clamp(lod - level, 0, 4));
        light += (1 - blocked) * v.rgb;
        blocked += (1 - blocked) * v.a;
        d += diameter * 0.7;
    }
    return float4(light, blocked);
}

// The previous frame's colour where it shows the far plane in this world direction (a = 1), else a = 0.
float4 EnvSkySeen(float3 dir)
{
    float3 v = float3(dot(dir, float3(toWorldX.x, toWorldY.x, toWorldZ.x)), dot(dir, float3(toWorldX.y, toWorldY.y, toWorldZ.y)),
                      -dot(dir, float3(toWorldX.z, toWorldY.z, toWorldZ.z)));
    if (v.z > 0.05)
    {
        float2 ndc = v.xy / (v.z * invProj);
        float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        float farPlane = depthParams.y / (1 + depthParams.x);
        if (all(uv > 0.002) && all(uv < 0.998) && Distance(uv) > farPlane * 0.9)
        {
            float4 c = srcTex.SampleLevel(linearClamp, uv, 0);
            if (Finite(c))
                return float4(c.rgb, 1);
        }
    }
    return 0;
}

float3 EnvColour(float3 dir, float3 avg, float halfTan)
{
    float3 O = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    dir = normalize(dir);
    float4 t = EnvTrace(O, dir, halfTan);
    float4 sky = historyTex.Load(int3(EnvTexel(dir, 7), 0));
    return t.rgb * voxParams.w + (1 - t.a) * (sky.a > 0 ? sky.rgb : avg);
}

[numthreads(8, 8, 1)]
void envmap(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    float2 suv = float2((gi % 8 + 0.5) / 8, (gi / 8 + 0.5) / 8 * 0.6);
    float farPlane = depthParams.y / (1 + depthParams.x);
    float4 sc = srcTex.SampleLevel(linearClamp, suv, 0);
    envSky[gi] = Distance(suv) > farPlane * 0.9 && Finite(sc) ? float4(sc.rgb, 1) : 0;
    GroupMemoryBarrierWithGroupSync();
    float4 sum = 0;
    for (uint k = 0; k < 64; k++)
        sum += envSky[k];
    uint square = id.x / kEnvSize;
    if (square > 12 || id.y >= kEnvSize)
        return;
    float4 prev = historyTex.Load(int3(id.xy, 0));
    if (!Finite(prev))
        prev = 0;
    float3 avg = sum.w > 0 ? sum.rgb / sum.w : prev.a > 0 && square >= 7 ? prev.rgb : float3(0.05, 0.05, 0.05);
    float2 f = (float2(id.x % kEnvSize, id.y) + 0.5) / kEnvSize;
    if (square >= 7)
    {
        float4 seen = EnvSkySeen(normalize(EnvFaceDir(square - 7, f)));
        outTex[id.xy] = seen.a > 0 ? float4(prev.a >= 1 ? lerp(prev.rgb, seen.rgb, 0.25) : seen.rgb, 1)
                        : prev.a >= 1 || sum.w == 0 ? prev : float4(avg, 0.5);
        return;
    }
    float3 c;
    if (square < 6)
        c = EnvColour(EnvFaceDir(square, f), avg, 0.08);
    else
    {
        c = 0;
        [unroll] for (int k = 0; k < 4; k++)
        {
            float2 xy = f - float2(k & 1, k >> 1);
            c += EnvColour(float3(xy, sqrt(saturate(1 - dot(xy, xy)))), avg, 0.25) * 0.25;
        }
    }
    c = 1 - exp(-max(c, 0) * kEnvExposure);
    float3 o = prev.a > 0 ? lerp(prev.rgb, c, 0.5) : c;
    outTex[id.xy] = float4(o, 1);
    // The textures the materials get, with the panel's strengths (reproj._11: the materials' own maps, ._12: every
    // material's): squares 13..19 (cube and 2D map), 20..25 (the cube for every material).
    outTex[uint2(id.x + 13 * kEnvSize, id.y)] = float4(o * reproj._11, 1);
    if (square < 6)
        outTex[uint2(id.x + 20 * kEnvSize, id.y)] = float4(o * reproj._12, 1);
}

// GI v2 (CRYMT_GI_PROBES=1, the GI page's "GI method", visAreas[0].w): irradiance probes in the manner of DDGI (Majercik
// et al. 2019) whose rays go through the voxel grid's cells. kProbeN probes per cascade (CascadeSpacing) around the camera,
// toroidal like the voxel levels. A probe's light (rgb, as the cones') and occlusion (a) over the sphere: an octahedral
// tile of 6 x 6 texels with a border of one (8 x 8) in an atlas of 128 x 128 tiles; the rows under them hold each probe's
// state: xyz its world probe coordinate mod 1024, w 1 + the share of its rays that hit within 0.35 m (a probe inside a
// wall), 0 never traced. Each frame a quarter of the probes, and every probe new to the grid, trace 64 rays (spherical
// Fibonacci, turned per frame) and blend them in; the others are copied (two atlases in turn: cs_5_0 cannot read a
// float4 UAV). Phase 1: no visibility test yet (light can still leak through a wall between a pixel and a probe).
static const int3 kProbeN = int3(32, 32, 16);
// Cascades (the user, 2026-11-14: blotches on ceilings with probes 1 m apart, 2-3 to a room's height): 0 the near one,
// 0.5 m apart (+-8 m, +-4 m up), 1 the outer one, 1 m (+-16 m, +-8 m up); the cones beyond. Each cascade has its own
// 1024 columns of the atlases (tiles, and its states and offsets in the rows under them).
static const int kProbeCascades = 2;
float CascadeSpacing(int c) { return c == 0 ? 0.5 : 1.0; }
static const float2 kProbeAtlas = float2(1024 * kProbeCascades, 1152);
// Phase 2, visibility (DDGI's): per probe the mean distance to what its rays hit and its square over the sphere, a tile
// laid out as the light's (6 x 6 and a border, 1024 x 1024 RG16F); distances clamped at 1.5 x the probe cell's diagonal,
// the rays weighted by cos^8 around the texel's direction. A pixel takes a probe by Chebyshev's bound on the chance that
// nothing stands between them (a probe behind a wall gets almost no weight). DDGI's 14 x 14 at cos^50 cost ~1.5 ms here
// (256 texels x 64 rays x pow a probe) and was undersampled by 64 rays anyway.
static const float2 kProbeDepthAtlas = float2(1024 * kProbeCascades, 1024);
static const float kProbeDepthSharpness = 8;
float ProbeMaxDistance(float sp) { return 1.5 * 1.7320508 * sp; }
groupshared float4 g_probeRay[64]; // a ray's light and occlusion
groupshared float4 g_probeDir[64]; // its direction; w its distance (clamped)

int3 ProbeOrigin(int c) { return (int3)floor(float3(toWorldX.w, toWorldY.w, toWorldZ.w) / CascadeSpacing(c)) - kProbeN / 2; }
float3 ProbePos(int3 wc, int c) { return (wc + 0.5) * CascadeSpacing(c); }
int ProbeSlot(int3 wc)
{
    int3 s = wc & (kProbeN - 1); // powers of two
    return s.x + s.y * kProbeN.x + s.z * kProbeN.x * kProbeN.y;
}
int2 ProbeTile(int slot, int c) { return int2(slot & 127, slot >> 7) * 8 + int2(1024 * c, 0); }
int2 ProbeDepthTile(int slot, int c) { return ProbeTile(slot, c); }

// A border texel's interior one in a tile of (n + 2)^2: across an edge the octahedral neighbour, flipped; corners the
// opposite corner (DDGI's border copy).
int2 OctInterior(int2 t, int n)
{
    int2 ip = t - 1;
    if (ip.x < 0 || ip.x > n - 1)
    {
        ip.x = clamp(ip.x, 0, n - 1);
        ip.y = n - 1 - ip.y;
    }
    if (ip.y < 0 || ip.y > n - 1)
    {
        ip.y = clamp(ip.y, 0, n - 1);
        ip.x = n - 1 - ip.x;
    }
    return ip;
}
int2 ProbeStateTexel(int slot, int c) { return int2(1024 * c + (slot & 127), 1024 + (slot >> 7)); }
// The probe's offset from its cell's centre (xyz, metres, within 0.45 of the spacing; DDGI's relocation): a probe under
// the ground steps up towards its surface, one by a surface steps away from it.
int2 ProbeOffsetTexel(int slot, int c) { return int2(1024 * c + 128 + (slot & 127), 1024 + (slot >> 7)); }
float3 ProbeKey(int3 wc) { return float3(wc & 1023); }

float2 OctEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 s = float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    return n.z >= 0 ? n.xy : (1 - abs(n.yx)) * s;
}

float3 OctDecode(float2 o)
{
    float3 n = float3(o, 1 - abs(o.x) - abs(o.y));
    if (n.z < 0)
        n.xy = (1 - abs(n.yx)) * float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    return normalize(n);
}

// Ray i of 64 (spherical Fibonacci), turned by a rotation picked from the frame.
float3 ProbeRayDir(uint i, float f)
{
    const float phi = 6.2831853 * frac(i * 0.618034);
    const float z = 1 - (2 * i + 1) / 64.0;
    const float r = sqrt(saturate(1 - z * z));
    const float3 d = float3(r * cos(phi), r * sin(phi), z);
    const float3 h = frac(sin(float3(12.9898, 78.233, 37.719) * (f + 1)) * 43758.5453);
    const float3 axis = normalize(h * 2 - 1 + 1e-3);
    const float a = h.z * 6.2831853, c = cos(a), sn = sin(a);
    return d * c + cross(axis, d) * sn + axis * dot(axis, d) * (1 - c);
}

// A thin ray through the grid from O along dir: front-to-back over the cells at mip 0 of the finest level that holds the
// point (level 0 up to 6 m in half cells, 1 to 12 m, 2 to 24 m, then 3, a cell a step), the cells' light as the cones
// take it (L1 / six directions); rgb light, a occlusion faded with distance as the cones' (occRange); hit where it was
// half blocked (reach if never).
float4 RayTrace(float3 O, float3 dir, float reach, float jitter, out float hit)
{
    const float vs = voxParams.x;
    float3 light = 0;
    float blocked = 0, occ = 0, t = vs * 0.5 * (0.5 + jitter);
    hit = reach;
    // Level by distance: 0 up to 3 m, 1 to 8 m, 2 to 20 m, then 3 (6 / 12 / 24 m until 2026-11-14 ~07:00: the probes the
    // relocation moved off walls skim floors and walls, a mip-3 block by a surface is never empty, and level 0's half
    // cells to 6 m made the probes pass 0.5 -> 1.9 ms). One loop a level (unrolled: the level is a constant in each, so
    // a wave's rays sample one texture, not every level's in turn as in one loop with the level chosen per step); a ray
    // that leaves a level's grid goes on in the next. Precise rays (the panel's "Probe rays", to compare with the stage
    // before the speed-ups, 3.2 ms): levels to 6 / 12 / 24 m, half cells to 6 m, no skip over mip 2.
    const bool precise = visAreas[kProbeOptions].x > 0.5;
    const float halfTo = precise ? 6 : 1.5;
    [unroll] for (int level = 0; level < 4; level++)
    {
        const float tEnd = min(precise ? (level == 0 ? 6 : level == 1 ? 12 : level == 2 ? 24 : reach)
                                       : (level == 0 ? 3 : level == 1 ? 8 : level == 2 ? 20 : reach), reach);
        const float cell = vs * (1 << level);
        [loop] for (int i = 0; i < 64 && blocked < 0.95 && t < tEnd; i++)
        {
            const float3 X = O + dir * t;
            if (!InGrid(level, X, 0))
                break;
            // Empty space skipped: nothing in mip 3 here steps three cells at once, nothing in mip 2 one cell. Mip 3's
            // texels are 8 cells, so a wall within 3.5 cells still weighs at least 1/16 of its share (>= 1/8) in the
            // trilinear sample; mip 2's (4 cells) keep a wall within 1.5 cells at >= 1/8 of its share. A skip of two
            // cells over mip 2 could pass a wall with no weight (the stair came out 3-5 darker at 10:00).
            if (SampleGrid(level, X, 3).a < 0.002)
            {
                t += cell * 3;
                continue;
            }
            if (!precise && SampleGrid(level, X, 2).a < 0.002)
            {
                t += cell;
                continue;
            }
            float4 v = LoadGrid(level, X); // the cell itself (GridCell)
            if (v.a > 0.004) // the cells' light direction only where there is a cell
            {
                if (visAreas[0].z > 1.5)
                    v.rgb = SixToward(level, X, 0, dir);
                else if (visAreas[0].z > 0.5)
                    v.rgb *= max(1 - dot(LoadL1(level, X), dir), 0);
            }
            // Mip 0 keeps the cells' colour as it is (mip 1 premultiplies it by the opacity; the cones start at mip 1):
            // a nearly empty cell by a surface carried its full colour, and the probes over island's beach took 4-7 of
            // light from above at an occlusion of 0.1.
            v.rgb *= v.a;
            light += (1 - blocked) * v.rgb;
            const float near = occRange.x > 0 ? saturate((occRange.x - t) / (occRange.x * 0.5)) : 1;
            occ += (1 - blocked) * v.a * near;
            blocked += (1 - blocked) * v.a;
            if (blocked > 0.5 && hit >= reach)
                hit = t;
            t += level == 0 && t < halfTo ? cell * 0.5 : cell; // half cells near the probe only
        }
        // Past this level's grid or its distance: on in the next level from here (its loop ends at once when blocked).
    }
    return float4(light, occ);
}

// One group a probe slot (128 x 128 groups a cascade, z the cascade), a thread a ray and a texel of its tile. t2 the
// previous atlas, u0 this one.
[numthreads(8, 8, 1)]
void probes(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int c = gid.z;
    const float sp = CascadeSpacing(c);
    const int slot = gid.x + gid.y * 128;
    const int3 s = int3(slot % kProbeN.x, (slot / kProbeN.x) % kProbeN.y, slot / (kProbeN.x * kProbeN.y));
    const int3 origin = ProbeOrigin(c);
    const int3 wc = origin + ((s - origin) & (kProbeN - 1)); // the probe in the grid now at this slot
    const int2 st = ProbeStateTexel(slot, c), texel = ProbeTile(slot, c) + int2(tid.xy);
    const float4 state = historyTex.Load(int3(st, 0));
    const bool valid = state.w >= 1 && all(state.xyz == ProbeKey(wc)) && temporal > 0;
    // The outer cascade every 8th frame (precise rays: every 4th, as the near one).
    const uint period = c == 0 || visAreas[kProbeOptions].x > 0.5 ? 3u : 7u;
    const bool due = !valid || (((uint)slot + (uint)frame) & period) == 0;
    const int2 ot = ProbeOffsetTexel(slot, c);
    const float3 offset = valid ? historyTex.Load(int3(ot, 0)).xyz : 0;
    float4 ray = 0;
    float3 dir = 0;
    float hit = 0;
    if (due)
    {
        dir = ProbeRayDir(gi, frame + c * 0.5);
        ray = RayTrace(ProbePos(wc, c) + offset, dir, voxParams.z, frac(gi * 0.618034 + frame * 0.381966), hit);
    }
    // The distance kept up to 16 m (the classification below asks whether the ray met anything at all); the
    // visibility's moments clamp it at ProbeMaxDistance. Backface hits by the cells' L1 direction were tried and
    // dropped: a thin roof lit from outside has its L1 pointing out as the ground has (probes under a shed's roof counted
    // as inside and were moved through it into the sun: blotches on ceilings), and a test of thickness missed the
    // probes under the beach (the ground's fill is thinner than three cells).
    g_probeRay[gi] = ray;
    g_probeDir[gi] = float4(dir, min(hit, 16));
    GroupMemoryBarrierWithGroupSync();
    const float4 old = historyTex.Load(int3(texel, 0));
    const int2 dt = ProbeDepthTile(slot, c) + int2(tid.xy); // the distances' texel, laid out as the light's
    const float2 oldM = srcTex.Load(int3(dt, 0)).xy;
    if (!due)
    {
        outTex[texel] = old;
        probeDepthOut[dt] = float4(oldM, 0, 1);
        if (gi == 0)
        {
            outTex[st] = state;
            outTex[ot] = float4(offset, 0);
        }
        return;
    }
    // The texel's interior one (the border copies the octahedral neighbour across the edge) and its direction: light by
    // cosine weights, distances by cos^8.
    const float3 n = OctDecode((OctInterior(int2(tid.xy), 6) + 0.5) / 6 * 2 - 1);
    float4 sum = 0;
    float weights = 0;
    float2 m = 0;
    float mw = 0;
    const float maxDistance = ProbeMaxDistance(sp);
    [loop] for (int r = 0; r < 64; r++)
    {
        const float w = max(dot(n, g_probeDir[r].xyz), 0);
        sum += g_probeRay[r] * w;
        weights += w;
        const float c2 = w * w, c4 = c2 * c2, wd = c4 * c4; // cos^8 (kProbeDepthSharpness)
        const float dist = min(g_probeDir[r].w, maxDistance);
        m += float2(dist, dist * dist) * wd;
        mw += wd;
    }
    const float4 fresh = sum / max(weights, 1e-4);
    const float2 freshM = mw > 1e-4 ? m / mw : oldM;
    probeDepthOut[dt] = float4(valid ? lerp(freshM, oldM, 0.95) : freshM, 0, 1);
    // Hysteresis 0.95 (64 rays a fourth frame: ~1300 rays' worth; at 0.8 the probes boiled and one frame's look at the
    // stair varied by 2.5 of 255 between runs). A faster blend on big changes (0.6 when the light doubled or halved) made
    // dim probes jump at their updates: the noise of 64 rays crossed that line (the user: voxels light up and go out).
    outTex[texel] = valid ? lerp(fresh, old, 0.95) : fresh;
    if (gi == 0)
    {
        // Inside geometry: most rays end within 0.35 m (in a wall); or under the ground: the rays going up nearly all
        // meet the ground's underside within 16 m while those going down nearly all meet nothing (the grid is empty below
        // the ground's fill). Under a roof the floor stops the rays going down.
        float closeShare = 0, frontMin = 1e9, upRays = 0, upClosed = 0, upMin = 1e9, downRays = 0, downOpen = 0;
        float3 frontDir = 0;
        [loop] for (int k = 0; k < 64; k++)
        {
            const float d = g_probeDir[k].w, dz = g_probeDir[k].z;
            closeShare += d < 0.35 * sp ? 1 : 0;
            if (d < frontMin)
            {
                frontMin = d;
                frontDir = g_probeDir[k].xyz;
            }
            if (dz > 0.3)
            {
                upRays++;
                if (d < 15.9)
                {
                    upClosed++;
                    upMin = min(upMin, d);
                }
            }
            if (dz < -0.3)
            {
                downRays++;
                downOpen += d >= 15.9 ? 1 : 0;
            }
        }
        const bool underground = upClosed > 0.8 * upRays && downOpen > 0.8 * downRays && upRays > 0 && downRays > 0;
        // Kept slowly (0.9): a probe by a wall whose share of 64 rays wavered went on and off.
        const float inside = underground ? 1 : saturate(closeShare / 48);
        outTex[st] = float4(ProbeKey(wc), 1 + (valid ? lerp(inside, state.w - 1, 0.9) : inside));
        // Relocation (DDGI's, by our classification): under the ground up through it (to the surface if it is within
        // reach), else away from a surface nearer than 0.3 of the spacing; within 0.45 of the spacing of the centre.
        float3 moved = offset;
        if (underground)
            moved.z += upMin + 0.25 * sp;
        else if (frontMin < 0.3 * sp)
            moved -= frontDir * (0.3 * sp - frontMin);
        outTex[ot] = float4(clamp(moved, -0.45 * sp, 0.45 * sp), 0);
    }
}

// GI v2: the light (rgb) and occlusion (a) at P for the normal N from the 8 probes around it, with DDGI's weights
// (trilinear, the side the normal faces, Chebyshev visibility from the probe's distances, probes inside walls nearly
// out). The point is moved off the surface towards the normal and the camera (DDGI's bias: a surface's own distance
// otherwise fails the visibility). inside: how far P lies in the probe grid (0 at its edge, 1 from two probes in), for
// the blend with the cones.
float4 ProbeLight(float3 P, float3 N, int c, out float inside)
{
    const float sp = CascadeSpacing(c);
    const int3 origin = ProbeOrigin(c);
    const float3 V = normalize(float3(toWorldX.w, toWorldY.w, toWorldZ.w) - P);
    const float3 Pb = P + (N * 0.2 + V * 0.8) * (0.3 * sp);
    const float3 g = Pb / sp - 0.5 - origin;
    const float3 room = min(g, (kProbeN - 1) - g);
    inside = saturate(min(room.x, min(room.y, room.z)) / 2);
    if (inside <= 0)
        return 0;
    const int3 base = (int3)floor(g);
    const float3 f = g - base;
    const float2 oct = OctEncode(N) * 0.5 + 0.5;
    float4 sum = 0;
    float weights = 0;
    [unroll] for (int i = 0; i < 8; i++)
    {
        const int3 off = int3(i & 1, (i >> 1) & 1, i >> 2);
        const int3 wc = origin + base + off;
        const int slot = ProbeSlot(wc);
        const float4 state = probeTex.Load(int3(ProbeStateTexel(slot, c), 0));
        if (state.w < 1 || any(state.xyz != ProbeKey(wc)))
            continue;
        const float3 tri = lerp(1 - f, f, (float3)off);
        const float3 probePos = ProbePos(wc, c) + probeTex.Load(int3(ProbeOffsetTexel(slot, c), 0)).xyz; // relocated
        const float facing = (dot(normalize(probePos - P), N) + 1) * 0.5;
        float w = (facing * facing + 0.2) * lerp(1, 0.02, saturate((state.w - 1.4) / 0.4));
        // Visibility: the biased point's distance from the probe against the probe's distances that way.
        const float3 fromProbe = Pb - probePos;
        const float dist = length(fromProbe);
        const float2 dOct = OctEncode(fromProbe / max(dist, 1e-4)) * 0.5 + 0.5;
        const float2 m = probeDepthTex.SampleLevel(linearClamp, (ProbeDepthTile(slot, c) + 1 + dOct * 6) / kProbeDepthAtlas, 0).xy;
        const float variance = abs(m.y - m.x * m.x);
        const float over = max(dist - m.x, 0);
        const float chebyshev = dist <= m.x ? 1 : variance / (variance + over * over);
        if (visAreas[0].w > 0) // negative: no visibility test (CRYMT_GI_PROBEVIS=0, to measure what it stops)
            w *= max(chebyshev * chebyshev * chebyshev, 0.05);
        // Tiny weights crushed (DDGI): a sliver of a probe's weight no longer lifts a dark corner.
        if (w < 0.2)
            w *= w * w / 0.04;
        w *= tri.x * tri.y * tri.z;
        sum += probeTex.SampleLevel(linearClamp, (ProbeTile(slot, c) + 1 + oct * 6) / kProbeAtlas, 0) * w;
        weights += w;
    }
    if (weights <= 0)
    {
        inside = 0;
        return 0;
    }
    return sum / weights;
}

[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)giSize.x || id.y >= (uint)giSize.y)
        return;
    float2 uv = GiDepthUv(id.xy);
    float4 result = 0;
    bool thin = false;
    float d0 = depthTex.SampleLevel(pointClamp, uv, 0);
    if (d0 >= kWeaponDepth && d0 < 0.99999)
    {
        float3 P = ViewPos(uv);
        float2 px = GiScale() / fullSize; // one GI texel: smoother normals than neighbouring depth pixels
        float3 pr = ViewPos(uv + float2(px.x, 0)), pl = ViewPos(uv - float2(px.x, 0));
        float3 pd = ViewPos(uv + float2(0, px.y)), pu = ViewPos(uv - float2(0, px.y));
        // A thin feature in front (a grass blade, twig, leaf edge a texel or two wide): the neighbours on both sides along
        // x or y lie farther by a step. Its normal from depth is arbitrary (both differences cross to what is behind),
        // so its occlusion was chance: the cones lifted along it started inside the bush (black) or outside it (full
        // ambient), bright strokes on black shade under canopy that moved with the jitter and the wind.
        float jump = 0.05 * P.z + 0.05;
        thin = ((uint)options & 256) != 0 && ((pr.z - P.z > jump && pl.z - P.z > jump) || (pd.z - P.z > jump && pu.z - P.z > jump));
        float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
        float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
        float3 N = normalize(cross(dx, dy));
        // A wide estimate (central differences three GI texels each way) averages across the faces of low-poly round
        // meshes, where the narrow one is flat per face (faces showed in the GI). Used where each pair's midpoint lies on
        // the surface (within 15% of the pair's span: no crease or step in the window); options & 8: narrow only.
        if (((uint)options & 8) == 0)
        {
            float2 wide = px * 3;
            float3 wr = ViewPos(uv + float2(wide.x, 0)), wl = ViewPos(uv - float2(wide.x, 0));
            float3 wd = ViewPos(uv + float2(0, wide.y)), wu = ViewPos(uv - float2(0, wide.y));
            float3 sx = wr - wl, sy = wd - wu;
            bool smooth = length((wr + wl) * 0.5 - P) < 0.15 * length(sx) && length((wd + wu) * 0.5 - P) < 0.15 * length(sy);
            float3 Nw = cross(sx, sy);
            if (smooth && dot(Nw, Nw) > 1e-12)
                N = normalize(Nw);
        }
        if (dot(N, P) > 0)
            N = -N; // facing the camera
        float2 radiusUv = min(radius / (P.z * invProj) * 0.5, 0.15);
        float noise = frac(52.9829189 * frac(dot(float2(id.xy), float2(0.06711056, 0.00583715))) + ((uint)options & 1 ? 0 : frame * 0.618034));
        float3 indirect = 0, toned = 0;
        float occ = 0, weights = 0;
        // Slices: 4 directions, or 2 (options & 32, turned with the frame's noise; the filter and the history average
        // them), each both ways; steps by the radius on screen: one per 1.5 GI texels, 2 to 6 (distant points have a
        // small radius in pixels; the trace was 0.57 ms of screen part at 5120x1440).
        int dirs = ((uint)options & 32) ? 2 : 4;
        int traced = voxParams.y == 2 ? 0 : dirs; // the cones' part only: the screen part is not used
        int steps = clamp((int)ceil(length(radiusUv * fullSize) / (1.5 * GiScale())), 2, 6);
        // Visibility bitmask (Therrien et al. 2023): per slice 32 sectors of the hemisphere around the normal, spaced
        // by the projected cosine (each one the same share of irradiance); a sample occupies the sectors between its
        // front and its back (pushed away from the camera by a thickness), occludes them once and gives its light only
        // to the sectors it is the first to cover: occlusion and light from the same visibility, no light through
        // thin objects counted twice. CRYMT_GI_SCREENCLASSIC=1 (debug 5): the horizon method before.
        bool classic = debug == 5;
        float3 Vc = normalize(-P);
        float thickness = max(radius, 0.5);
        float sinMax = sin(1.5707963 - kBias);
        [loop] for (int k = 0; k < traced; k++)
        {
            float a = (k + noise) * (3.14159265 / dirs);
            float2 dir = float2(cos(a), sin(a));
            float3 dvec = ViewPosAt(uv + dir / fullSize, d0) - P; // the slice's direction at this depth
            float3 ortho = normalize(dvec - dot(dvec, Vc) * Vc + 1e-6);
            float3 axis = cross(ortho, Vc);
            float3 pn = N - axis * dot(N, axis);
            float n = (dot(ortho, pn) < 0 ? -1.0 : 1.0) * acos(saturate(dot(pn, Vc) / max(length(pn), 1e-5)));
            uint occluded = 0;
            [unroll] for (int side = -1; side <= 1; side += 2)
            {
                float horizon = kBias; // cosine to the normal of the highest sample so far
                [loop] for (int s = 1; s <= steps; s++)
                {
                    float2 offset = side * dir * radiusUv * ((s - noise * 0.5) / steps);
                    if (dot(offset * fullSize, offset * fullSize) < 9) // closer than 3 pixels: depth precision only
                        continue;
                    float2 suv = Snap(uv + offset);
                    if (any(suv < 0) || any(suv > 1))
                        break;
                    float sd = depthTex.SampleLevel(pointClamp, suv, 0);
                    if (sd < kWeaponDepth)
                        continue;
                    float3 S = ViewPosAt(suv, sd);
                    float3 v = S - P;
                    float len2 = dot(v, v);
                    float c = dot(N, v * rsqrt(max(len2, 1e-8)));
                    float fall = saturate(1 - len2 / (radius * radius));
                    uint fresh = 0;
                    if (classic)
                    {
                        if (!(c > horizon))
                            continue;
                    }
                    else
                    {
                        if (fall <= 0)
                            continue;
                        // The sample's front and back as angles in the slice from the view direction, then from the
                        // normal (u), then as the projected-cosine position 0..1 (the edges kBias off the plane).
                        float3 back = S + normalize(S) * thickness;
                        float uf = side * acos(clamp(dot(v * rsqrt(max(len2, 1e-8)), Vc), -1, 1)) - n;
                        float ub = side * acos(clamp(dot(normalize(back - P), Vc), -1, 1)) - n;
                        float t0 = saturate((sin(clamp(uf, -1.5707963, 1.5707963)) / sinMax + 1) * 0.5);
                        float t1 = saturate((sin(clamp(ub, -1.5707963, 1.5707963)) / sinMax + 1) * 0.5);
                        uint lo = (uint)floor(min(t0, t1) * 32), hi = (uint)ceil(max(t0, t1) * 32);
                        uint count = hi > lo ? hi - lo : 0;
                        uint bits = count >= 32 ? 0xFFFFFFFFu : (((1u << count) - 1u) << lo);
                        fresh = bits & ~occluded;
                        occluded |= bits;
                        if (fresh == 0)
                            continue;
                    }
                    // The colour is last frame's: read it where this point was then.
                    float4 sc = mul(float4(S.xy, -S.z, 1), reproj);
                    float2 cuv = float2(sc.x, -sc.y) / sc.w * 0.5 + 0.5;
                    float3 colour = srcTex.SampleLevel(linearClamp, cuv, 0).rgb;
                    if (all(cuv >= 0) && all(cuv <= 1) && Finite(float4(colour, 0)))
                    {
                        float3 lit = clamp(colour, 0, 4.0);
                        float w;
                        if (classic)
                            w = (c - horizon) * fall;
                        else
                        {
                            // Less of our own light fed back: the colour there holds last frame's GI times its albedo
                            // (taken as 0.5); indoors that loop lit the corners (wall to corner to wall, times the bounce).
                            float3 own = historyTex.SampleLevel(linearClamp, cuv, 0).rgb;
                            lit = max(lit - 0.5 * max(own, 0), 0);
                            w = countbits(fresh) / 32.0;
                        }
                        indirect += lit * w;
                        toned += lit / (1 + max(lit.r, max(lit.g, lit.b))) * w;
                        weights += w;
                    }
                    if (classic)
                        horizon = lerp(horizon, c, fall);
                }
                if (classic)
                    occ += (horizon - kBias) / (1 - kBias);
            }
            if (!classic)
                occ += countbits(occluded) / 32.0 * 2; // both sides in one mask: counted as two halves
        }
        // Bright spots among the samples (a sunlit patch seen from the shade) make single texels flare, white flicker in
        // motion once the bounce is strong: the samples are averaged as c / (1 + max(c)) and turned back (Karis), so
        // one bright sample weighs no more than a dull one while an evenly lit area keeps its light. options & 4: plain.
        if (!((uint)options & 4) && weights > 0)
        {
            float3 mean = toned / weights;
            indirect = mean / max(1 - max(mean.r, max(mean.g, mean.b)), 0.05) * weights;
        }
        // Near the GI alone, then over 1.5 x that the engine's baked occlusion takes over (the shader patch's
        // CryMT_GIBakedMix, the same window): full up to options >> 16 (5 bits) x 2 metres, none from 2.5 x that on.
        float fadeStart = (float)(((uint)options >> 16) & 31u) * 2;
        float fade = fadeStart > 0 ? saturate((fadeStart * 2.5 - P.z) / (fadeStart * 1.5)) : saturate(1 - P.z / maxDistance);
        float3 nearLight = indirect * (bounce / (dirs * (classic ? 2 : 1)));
        float nearOcc = occ / (dirs * 2);
        // With the GTAO passes (in place of the engine's SSAO) the near occlusion is theirs alone: this part keeps the
        // bounce light (two near occlusions darkened the creases twice).
        if (((uint)options & 512) != 0 && motionParams.y > 0)
            nearOcc = 0;
        if (voxParams.y > 0)
        {
            // Five cones of 60 degrees (the normal and four around it, turned per texel) through the voxel grid, from a
            // point lifted off the surface by two cells, from two cells on: the lowest edge of a side cone's first
            // sample stays about a cell above the surface (no self-occlusion); near geometry off screen counts too.
            float3 Pe = float3(P.xy, -P.z), Ne = float3(N.xy, -N.z);
            float3 Pw = float3(dot(float4(Pe, 1), toWorldX), dot(float4(Pe, 1), toWorldY), dot(float4(Pe, 1), toWorldZ));
            float3 Nw = normalize(float3(dot(Ne, toWorldX.xyz), dot(Ne, toWorldY.xyz), dot(Ne, toWorldZ.xyz)));
            float3 T = normalize(cross(abs(Nw.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0), Nw));
            float3 B = cross(Nw, T);
            float3 O = Pw + Nw * (voxParams.x * 2);
            float start = voxParams.x * 2;
            const int area = VisAreaAt(Pw);
            if (area >= 0 && visAreas[0].y > 0.5)
                SetRoom(area);
            // GI v2: the probes inside their grid, the cones beyond it and across its last two probes. |visAreas[0].w|:
            // the probes' strength, + 100 when the cones keep the occlusion everywhere and the probes give the light only
            // (the user, 2026-11-14: walking, surfaces the GI had darkened came out lighter where the probes took over:
            // a probe hangs up to a metre off the surface and misses the occlusion close to it).
            float probeIn = 0;
            const float probeW = abs(visAreas[0].w);
            const bool probeLightOnly = probeW >= 100;
            const float probeStrength = probeLightOnly ? probeW - 100 : probeW;
            // The near cascade (0.5 m) inside its grid, blended to the outer one (1 m) over its last two probes.
            float4 probe = 0;
            if (visAreas[0].w != 0)
            {
                float in0 = 0, in1 = 0;
                const float4 p0 = ProbeLight(Pw, Nw, 0, in0);
                const float4 p1 = in0 < 1 ? ProbeLight(Pw, Nw, 1, in1) : 0;
                probeIn = in0 + (1 - in0) * in1;
                probe = probeIn > 0 ? (p0 * in0 + p1 * (1 - in0) * in1) / probeIn : 0;
            }
            float3 farLight = 0;
            float farOcc = 0;
            if (probeIn < 1 || probeLightOnly)
            {
                float4 cone = ConeTrace(O, Nw, 0.577, start, voxParams.z, false, true, Pw, Nw, noise);
                farLight = cone.rgb;
                farOcc = cone.a;
                [unroll] for (int c = 0; c < 4; c++)
                {
                    float a = (c + noise) * 1.5707963;
                    float3 dir = normalize(Nw * 0.5 + (T * cos(a) + B * sin(a)) * 0.866);
                    cone = ConeTrace(O, dir, 0.577, start, voxParams.z, false, true, Pw, Nw, noise + c * 0.25);
                    farLight += cone.rgb * 0.5;
                    farOcc += cone.a * 0.5;
                }
                farLight *= voxParams.w / 3;
                farOcc /= 3;
            }
            farLight = lerp(farLight, probe.rgb * probeStrength, probeIn); // the probes' own strength (CRYMT_GI_PROBESTRENGTH)
            if (!probeLightOnly)
                farOcc = lerp(farOcc, probe.a, probeIn);
            // Indoors: a VisArea; else the cover above (houses without one): the roof texture half a metre off the
            // surface and, when the two strengths differ, a ray up leaned off the surface and turned per texel.
            float roof = area >= 0 ? 1 : 0;
            if (roof < 1 && ((uint)options & 128) != 0) // CRYMT_GI_ROOFCELLS=1
                roof = max(roof, RoofAt(Pw + Nw * (voxParams.x * 2)));
            if (roof < 1 && occRange.y != motionParams.w)
            {
                float ra = noise * 6.2831853;
                float3 up = normalize(float3(0, 0, 1) + Nw * 0.5 + (T * cos(ra) + B * sin(ra)) * 0.25);
                roof = max(roof, RoofCover(O, up, frac(noise + 0.5)));
            }
            if (debug == 4) // the indoor share (VisArea, roof) as occlusion
            {
                outTex[id.xy] = float4(0, 0, 0, roof * 0.8);
                return;
            }
            // Near occlusion (Num3 / CRYMT_GI_NEAROCC with the voxelized geometry): five short cones (2 m) from close to
            // the surface (voxParams2.y/z cells), occlusion only, each sample counting only for its part above the
            // surface's own plane (else the surface shades itself). Adds darkening under and beside nearby objects;
            // the cones above keep the light and the look of foliage.
            if (voxParams2.w > 0)
            {
                float3 On = Pw + Nw * (voxParams.x * voxParams2.y);
                float nearStart = voxParams.x * voxParams2.z;
                float near = ConeTrace(On, Nw, 0.577, nearStart, 2.0, true, true, Pw, Nw, noise).a;
                [unroll] for (int k = 0; k < 4; k++)
                {
                    float a = (k + noise + 0.5) * 1.5707963;
                    float3 dir = normalize(Nw * 0.5 + (T * cos(a) + B * sin(a)) * 0.866);
                    near += ConeTrace(On, dir, 0.577, nearStart, 2.0, true, true, Pw, Nw, noise + k * 0.25).a * 0.5;
                }
                farOcc = 1 - (1 - farOcc) * (1 - saturate(near / 3 * voxParams2.w));
            }
            // The grid's outer edge: the voxel part fades over the outer level's last 16 cells (32 m), else it ended in a
            // sharp line where the pixels left the grid (the user, village beach 2026-11-13: a hard edge that did not move
            // with the GI's distance).
            const float outer = saturate(EdgeCells(3, Pw, 0) / 16);
            farLight *= outer;
            farOcc *= outer;
            if (voxParams.y == 2)
            {
                nearLight = 0;
                nearOcc = 0;
            }
            if (voxParams.y == 3)
            {
                farLight = 0;
                farOcc = 0;
            }
            // The screen part's occlusion takes the AO strength (occlusion), the cones' their own (voxParams2.x).
            nearLight += (1 - nearOcc) * farLight;
            nearOcc = 1 - (1 - saturate(nearOcc * occlusion)) * (1 - saturate(farOcc * voxParams2.x));
            result = float4(nearLight, Strengthen(nearOcc, roof)) * fade;
        }
        else
            result = float4(nearLight, Strengthen(nearOcc * occlusion, 1)) * fade; // no grid: the strength everywhere
    }
    if (debug == 2) // CRYMT_GI_DEBUG=2: the occlusion only, 3: the bounce light only (measurements), 4: the VisArea mask
        result.rgb = 0;

    if (debug == 3)
        result.a = 0;
    // Thin: marked for the filter with the occlusion stored as -1 - a (a in 0..1); the filter gives it its surroundings'.
    outTex[id.xy] = thin ? float4(result.rgb, -1 - result.a) : result;
}

// 5x5 depth-aware blur of the raw result, then the blend with the previous GI where this point was seen last frame
// (reprojected with the camera motion; no history where the previous distance there differs: disocclusion).
[numthreads(8, 8, 1)]
void filter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)giSize.x || id.y >= (uint)giSize.y)
        return;
    float2 uv = (id.xy + 0.5) / giSize;
    if (depthTex.SampleLevel(pointClamp, GiDepthUv(id.xy), 0) < kWeaponDepth)
    {
        outTex[id.xy] = 0;
        distOut[id.xy] = 0;
        return;
    }
    float zc = Distance(GiDepthUv(id.xy));
    float4 sum = 0, sum2 = 0, around = 0;
    float weights = 0, aroundWeights = 0;
    float4 own = srcTex.SampleLevel(pointClamp, uv, 0);
    bool thinHere = own.a < -0.5;
    [unroll] for (int y = -2; y <= 2; y++)
        [unroll] for (int x = -2; x <= 2; x++)
        {
            float2 suv = uv + float2(x, y) / giSize;
            float spatial = 3 - 0.5 * (abs(x) + abs(y));
            float w = exp(-abs(Distance(GiDepthUv(int2(id.xy) + int2(x, y))) - zc) / (0.03 * zc + 0.05)) * spatial;
            float4 s = srcTex.SampleLevel(pointClamp, suv, 0);
            if (s.a < -0.5)
            {
                // A thin feature's own value counts only for itself, when nothing around is a surface.
                if (x == 0 && y == 0)
                {
                    s.a = -1 - s.a;
                    sum += s * w * 0.01;
                    sum2 += s * s * w * 0.01;
                    weights += w * 0.01;
                }
                continue;
            }
            sum += s * w;
            sum2 += s * s * w;
            weights += w;
            around += s * spatial;
            aroundWeights += spatial;
        }
    if (thinHere && aroundWeights > 0)
    {
        // The surfaces around it, whatever their depth (it sits on or in front of them: their cover is its cover).
        float4 a = around / aroundWeights;
        sum = a;
        sum2 = a * a;
        weights = 1;
    }
    float3 V = ViewPos(GiDepthUv(id.xy));
    float4 prevClip = mul(float4(V.xy, -V.z, 1), reproj);
    float2 prevUv = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5;
    float history = 0;
    if (prevClip.w > 0 && all(prevUv >= 0) && all(prevUv <= 1))
    {
        // Seen last frame if one of the 2x2 texels the history is filtered from was at this distance (one texel
        // alone misses on distant slopes, where the distance changes fast from row to row).
        int2 p0 = int2(floor(prevUv * giSize - 0.5));
        float tolerance = 0.04 * prevClip.w + 0.05;
        [unroll] for (int j = 0; j < 4; j++)
            if (abs(distPrev[clamp(p0 + int2(j & 1, j >> 1), 0, int2(giSize) - 1)] - prevClip.w) < tolerance)
                history = temporal;
    }
    // History kept within the spread of this frame's neighbourhood: what it no longer has (moved shadows, revealed
    // surfaces) does not trail.
    float4 current = sum / max(weights, 1e-4);
    float4 sigma = sqrt(max(sum2 / max(weights, 1e-4) - current * current, 0));
    float4 result = current;
    float4 previous = historyTex.SampleLevel(linearClamp, prevUv, 0);
    if (history > 0 && Finite(previous))
        result = lerp(current, clamp(previous, current - 1.5 * sigma - 0.01, current + 1.5 * sigma + 0.01), history);
    outTex[id.xy] = Finite(result) ? result : 0;
    if (debug == 1 && history == 0 && V.z < maxDistance) // CRYMT_GI_DEBUG=1: magenta where there was no history
        outTex[id.xy] = float4(4, 0, 4, 1);
    distOut[id.xy] = V.z;
}

// Full-size GI for the engine's pixel shaders: each pixel blends the GI texels around it (3x3, a tent over their depth
// samples, which sit on every second pixel) weighted by how close their distance is to its own, so fine geometry (a
// character's straps, leaves, gravel) no longer takes the GI of the surface behind it; with no texel at its distance it
// takes the closest one. Weapon and sky pixels get none. t0 depth, t1 filtered GI, u0 out, u1 the GI texels' distances.
[numthreads(8, 8, 1)]
void upsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float d = depthTex.Load(int3(id.xy, 0));
    // The shader patch's flags: a + 2 everywhere the GI is in effect, sky included (its pixels use no ambient), so
    // filtering between texels never mixes them; which of the engine's terms it replaces in the top-left texel, a / 2
    // (bits: 1 SSAO, 2 the objects' RAM, 4 their sky amount, 8 TerrainAO near the camera, 16 the terrain's vertex AO,
    // 32 blended particles) plus 64 x half the metres up to which the GI replaces TerrainAO, rgb 0.
    uint mask = (((uint)options & 512) ? 1u : 0u) | (((uint)options >> 10) & 30u);
    float flags = mask ? 2.0 : 0.0;
    if (id.x == 0 && id.y == 0)
    {
        // 32: the particles' GI (options 2^21); 64 x half TerrainAO's distance (options >> 16, 5 bits): a / 2 < 2048.
        uint control = mask | ((((uint)options >> 21) & 1u) << 5) | ((((uint)options >> 16) & 31u) << 6);
        outTex[id.xy] = float4(0, 0, 0, 2.0 * control);
        return;
    }
    if (d < kWeaponDepth)
    {
        // The first-person view: the camera's GI with the flags, or nothing (the engine's own terms) without it.
        outTex[id.xy] = ((uint)options & 16) ? historyTex.Load(int3(0, 0, 0)) + float4(0, 0, 0, flags) : 0;
        return;
    }
    if (d >= 0.99999)
    {
        outTex[id.xy] = float4(0, 0, 0, flags);
        return;
    }
    float z = depthParams.y / (d + depthParams.x);
    float tolerance = 0.03 * z + 0.05;
    int f = GiScale();
    int2 g0 = int2(id.xy) / f;
    int2 r = int2(id.xy) - g0 * f;
    float4 sum = 0;
    float weights = 0, nearest = 1e30;
    float4 nearestValue = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            int2 g = clamp(g0 + int2(x, y), 0, int2(giSize) - 1);
            float2 delta = float2(r - int2(x, y) * f);
            float spatial = max(0, 1 - abs(delta.x) / (1.25 * f)) * max(0, 1 - abs(delta.y) / (1.25 * f));
            if (spatial <= 0)
                continue;
            float gz = distOut[g];
            float4 v = srcTex.Load(int3(g, 0));
            float dz = abs(gz - z);
            float w = spatial * exp(-dz / tolerance);
            sum += v * w;
            weights += w;
            if (dz < nearest)
            {
                nearest = dz;
                nearestValue = v;
            }
        }
    outTex[id.xy] = (weights > 1e-3 ? sum / weights : nearestValue) + float4(0, 0, 0, flags);
}

// GI for the first-person view (weapon, hands, objects held: drawn with their own camera into depth under kWeaponDepth,
// where the screen part has nothing to go by): one value for the frame, 32 cones from the eye over the sphere through
// the voxel grid (mean light times the cones' strength, mean occlusion times theirs), blended over frames in u0 (1x1:
// the previous value in, the new out). The upsample gives it to those pixels (options & 16).
groupshared float4 cameraCones[32];
[numthreads(32, 1, 1)]
void camera(uint3 id : SV_DispatchThreadID)
{
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float k = id.x + 0.5;
    float z = 1 - 2 * k / 32;
    float r = sqrt(saturate(1 - z * z));
    float phi = k * 2.39996323 + frac(frame * 0.618034) * 6.2831853;
    float3 dir = float3(r * cos(phi), r * sin(phi), z);
    cameraCones[id.x] = ConeTrace(eye, dir, 0.577, voxParams.x * 2, voxParams.z, false, false, eye, dir, frac(frame * 0.618034 + k * 0.1));
    GroupMemoryBarrierWithGroupSync();
    if (id.x != 0)
        return;
    float4 sum = 0;
    for (int i = 0; i < 32; i++)
        sum += cameraCones[i];
    sum /= 32;
    float roof = max(VisInside(eye), ((uint)options & 128) != 0 ? RoofAt(eye) : 0); // blended over frames below
    float4 value = float4(sum.rgb * voxParams.w, Strengthen(sum.a * voxParams2.x, roof));
    float4 previous = outTex[uint2(0, 0)];
    outTex[uint2(0, 0)] = temporal > 0 && Finite(previous) ? lerp(value, previous, 0.9) : value;
}

// Motion vectors for DLSS (full size, screen units, previous minus current, camera motion included): t0 depth, t1 object
// motion, t2 bent vegetation, u0 out. Every point moves with the camera; objects the engine's motion blur pass drew
// (moving ones, and the first-person weapon and hands) by their own before, which has the camera's move in it (the
// pass's MotionBlurData is the previous view x the previous world matrix; the first-person view's are relative to the
// camera, so there it is the sway, bob and recoil). The weapon has no camera motion of its own (0 where not drawn).
// t1: motionParams.y 2 CryMT's target from the patched pass (xy before minus now, z distance / far, w 1 exact, 0.5 with
// the engine's shutter factor f still in, 0 not drawn); 1 a copy of the engine's own (rg (now - before) x f x (1 -
// distance / 200)^8, zeroed under 0.001; b distance / far; a its id, non-zero where drawn). radius: the frame's f.
[numthreads(8, 8, 1)]
void motion(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float2 uv = (id.xy + 0.5) / fullSize;
    float d = depthTex[id.xy];
    bool weapon = d < kWeaponDepth;
    float2 mv = 0;
    float3 V = 0;
    if (!weapon)
    {
        V = ViewPosAt(uv, d);
        float4 prevClip = mul(float4(V.xy, -V.z, 1), reproj);
        mv = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5 - uv;
    }
    // Taken only where the object is this pixel's (its distance matches the scene's; the weapon's is in its own range)
    // and the move stays under a quarter of the screen a frame.
    bool object = false;
    if (motionParams.y > 1.5)
    {
        float4 o = srcTex[id.xy];
        bool exact = o.w > 0.75;
        float2 obj = exact ? o.xy : o.xy / max(radius, 1e-3);
        float dist = o.z * motionParams.x;
        if ((exact || (o.w > 0.25 && radius > 1e-3)) && (weapon || abs(dist - V.z) < 0.03 * V.z + 0.05) && all(abs(obj) < 0.25) && all(obj == obj))
        {
            mv = obj;
            object = true;
        }
    }
    else if (motionParams.y > 0.5 && radius > 1e-3)
    {
        // History: until 2026-11-13 this read the target after the engine had reused it for a copy of the scene
        // (KeepObjectMotion copies it now right after its pass), and subtracted it from the camera's move as if it were the
        // object's own at f = 1 (it has the camera's move in it, times f: 0.72 at 60 fps).
        float4 v = srcTex[id.xy];
        float dist = v.z * motionParams.x;
        float fade = pow(1 - saturate(dist / 200), 8);
        float2 obj = fade > 0.002 ? -v.xy / (fade * radius) : 0;
        if (v.a != 0 && any(v.xy != 0) && fade > 0.002 && (weapon || abs(dist - V.z) < 0.03 * V.z + 0.05) && all(abs(obj) < 0.25) && all(obj == obj))
        {
            mv = obj;
            object = true;
        }
    }
    // Wind-bent vegetation (t2, the patched z-prepass: before minus now per 1/60 s, motionParams.z the frame's scale).
    if (!weapon && !object && motionParams.w > 0)
    {
        float2 bent = historyTex[id.xy].xy * motionParams.z;
        if (all(abs(bent) < 0.1) && all(bent == bent))
            mv += bent;
    }
    outTex[id.xy] = float4(mv, 0, 0);
}

// The scene colour for DLSS without negative, NaN or infinite components (Device::RunSanitize, CRYMT_DLSS_CLAMP=1):
// t1 the colour, u0 the copy DLSS reads. The engine's tone mapping clips such values to black; DLSS may not.
[numthreads(8, 8, 1)]
void sanitize(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float4 c = srcTex[id.xy];
    c.rgb = (c.rgb >= 0 && c.rgb < 65504) ? c.rgb : 0; // NaN fails both
    outTex[id.xy] = c;
}

// The same in place on u0 (CRYMT_DLSS_CLAMP=out: DLSS's output, before the engine's tone mapping reads it).
[numthreads(8, 8, 1)]
void sanitizeOut(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float4 c = outTex[id.xy];
    c.rgb = (c.rgb >= 0 && c.rgb < 65504) ? c.rgb : 0;
    outTex[id.xy] = c;
}

// GTAO in its own passes (Device::RunGtao; CRYMT_GI_GTAO strength, CRYMT_GI_GTAORADIUS metres; with the engine's SSAO
// replaced). At 5120x1440 the GI runs at a quarter of the size: the occlusion computed there was coarse and shimmered
// in the upsampling. Like the engine's SSAO: half size, a fixed 4x4 pattern of slice turns (no change from frame to
// frame), a depth-aware 4x4 blur that averages the pattern away, then a factor on the full-size GI.
// gtao: t0 depth, t1 the foliage mask (options 2^22: its pixels occlude nothing), t2 the sun's shadow mask (red;
// options 2^23: foliage in the shadow gets 0.2 of the occlusion: there the ambient is all its light and the occlusion
// showed too strong), u1 the occlusion. Jimenez et al. 2016 / XeGTAO: per slice the normal projected, horizons
// with a radius falloff, quadratic steps from a pixel out, the cosine-weighted visible arcs.
static const float kBayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float ZAt(int2 px)
{
    return depthParams.y / (depthTex.Load(int3(px, 0)) + depthParams.x);
}

// GTAO's size: full (options & 32768) or half. Its own history: options & 1024 (reprojected, 0.9).
int AoScale()
{
    return ((uint)options & 32768) ? 1 : 2;
}

bool AoHistory()
{
    return ((uint)options & 1024) != 0;
}

[numthreads(8, 8, 1)]
void gtao(uint3 id : SV_DispatchThreadID)
{
    int aoS = AoScale();
    int2 aoSize = int2((fullSize + aoS - 1) / aoS);
    if (any(int2(id.xy) >= aoSize))
        return;
    int2 px = min(int2(id.xy) * aoS, int2(fullSize) - 1);
    float2 uv = (px + 0.5) / fullSize;
    float d0 = depthTex.Load(int3(px, 0));
    if (d0 < kWeaponDepth || d0 >= 0.99999)
    {
        distOut[id.xy] = 0;
        return;
    }
    float3 P = ViewPosAt(uv, d0);
    // The normal from the full-size depth a pixel apart (the smaller difference each way).
    int2 lim = int2(fullSize) - 1;
    float3 pr = ViewPosAt((min(px + int2(1, 0), lim) + 0.5) / fullSize, depthTex.Load(int3(min(px + int2(1, 0), lim), 0)));
    float3 pl = ViewPosAt((max(px - int2(1, 0), 0) + 0.5) / fullSize, depthTex.Load(int3(max(px - int2(1, 0), 0), 0)));
    float3 pd = ViewPosAt((min(px + int2(0, 1), lim) + 0.5) / fullSize, depthTex.Load(int3(min(px + int2(0, 1), lim), 0)));
    float3 pu = ViewPosAt((max(px - int2(0, 1), 0) + 0.5) / fullSize, depthTex.Load(int3(max(px - int2(0, 1), 0), 0)));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0)
        N = -N;
    float gR = motionParams.x > 0 ? motionParams.x : radius;
    float2 gRUv = min(gR / (P.z * invProj) * 0.5, 0.15);
    float radiusPx = length(gRUv * fullSize);
    if (radiusPx < 2)
    {
        distOut[id.xy] = 0;
        return;
    }
    // A fixed pattern (the 4x4 blur averages it away), the same every frame: turns that moved per frame for DLAA to
    // accumulate made a still view shimmer (DLAA saw the picture change while nothing moved).
    float rot = (kBayer4[(id.x & 3) + (id.y & 3) * 4] + 0.5) / 16;
    float stepNoise = frac(rot * 7 + 0.37);
    float3 Vv = normalize(-P);
    float minS = 1.0 / radiusPx;
    float falloffMul = -1 / (gR * 0.615), falloffAdd = 0.385 / 0.615 + 1;
    const int slices = 2, gsteps = 6;
    float vis = 0;
    [unroll] for (int k = 0; k < slices; k++)
    {
        float a = (k + rot) * (3.14159265 / slices);
        float2 dir = float2(cos(a), sin(a));
        float3 dvec = ViewPosAt(uv + dir / fullSize, d0) - P; // the slice's direction at this depth
        float3 ortho = dvec - dot(dvec, Vv) * Vv;
        float3 axis = normalize(cross(ortho, Vv));
        float3 pn = N - axis * dot(N, axis);
        float pnLen = length(pn);
        float cosN = saturate(dot(pn, Vv) / max(pnLen, 1e-5));
        float n = (dot(ortho, pn) < 0 ? -1.0 : 1.0) * acos(cosN);
        float low0 = cos(n + 1.5707963), low1 = cos(n - 1.5707963);
        float hc0 = low0, hc1 = low1; // side 0: along dir, side 1: against
        [loop] for (int st = 0; st < gsteps; st++)
        {
            float sN = (st + stepNoise) / gsteps;
            sN = sN * sN + minS;
            float2 offset = dir * gRUv * sN;
            [unroll] for (int side = 0; side < 2; side++)
            {
                float2 suv = uv + (side == 0 ? offset : -offset);
                if (any(suv < 0) || any(suv > 1))
                    continue;
                int2 spx = min(int2(suv * fullSize), lim);
                float sd = depthTex.Load(int3(spx, 0));
                if (sd < kWeaponDepth || (((uint)options & 4194304) && srcTex.Load(int3(spx, 0)).x > 0.5))
                    continue;
                float3 dv = ViewPosAt((spx + 0.5) / fullSize, sd) - P;
                float dist = length(dv);
                float wgt = saturate(dist * falloffMul + falloffAdd);
                float shc = lerp(side == 0 ? low0 : low1, dot(dv / max(dist, 1e-5), Vv), wgt);
                if (side == 0)
                    hc0 = max(hc0, shc);
                else
                    hc1 = max(hc1, shc);
            }
        }
        float h0 = -acos(clamp(hc1, -1, 1)), h1 = acos(clamp(hc0, -1, 1));
        h0 = n + clamp(h0 - n, -1.5707963, 1.5707963);
        h1 = n + clamp(h1 - n, -1.5707963, 1.5707963);
        float sinN = sin(n);
        vis += pnLen * ((cosN + 2 * h0 * sinN - cos(2 * h0 - n)) + (cosN + 2 * h1 * sinN - cos(2 * h1 - n))) * 0.25;
    }
    float occlusion = saturate(1 - vis / slices);
    if (((uint)options & 8388608) && srcTex.Load(int3(px, 0)).x > 0.5)
        occlusion *= lerp(1, 0.2, saturate(historyTex.Load(int3(px, 0)).x));
    distOut[id.xy] = occlusion;
}

// The 4x4 blur (the pattern's size), depth-aware, then blended with the previous frame's (t2, reprojected with the
// camera; kept only where the distance it stored matches, and within the spread of this frame's samples), 0.9: about
// ten frames. t0 depth, t1 the raw occlusion, u0 the history out (x occlusion, y distance). temporal 0: a camera cut.
[numthreads(8, 8, 1)]
void gtaoBlur(uint3 id : SV_DispatchThreadID)
{
    int aoS = AoScale();
    int2 aoSize = int2((fullSize + aoS - 1) / aoS);
    if (any(int2(id.xy) >= aoSize))
        return;
    int2 lim = int2(fullSize) - 1;
    int2 pc = min(int2(id.xy) * aoS, lim);
    float dc = depthTex.Load(int3(pc, 0));
    if (dc < kWeaponDepth || dc >= 0.99999)
    {
        outTex[id.xy] = float4(0, 0, 0, 0);
        return;
    }
    float zc = ZAt(pc);
    float sum = 0, sum2 = 0, weights = 0;
    [unroll] for (int y = -2; y <= 1; y++)
        [unroll] for (int x = -2; x <= 1; x++)
        {
            int2 t = clamp(int2(id.xy) + int2(x, y), 0, aoSize - 1);
            int2 p = min(t * aoS, lim);
            float d = depthTex.Load(int3(p, 0));
            if (d < kWeaponDepth || d >= 0.99999)
                continue;
            float w = exp(-abs(ZAt(p) - zc) / (0.02 * zc + 0.05));
            float v = srcTex.Load(int3(t, 0)).x;
            sum += v * w;
            sum2 += v * v * w;
            weights += w;
        }
    float current = weights > 0 ? sum / weights : 0;
    float sigma = weights > 0 ? sqrt(max(sum2 / weights - current * current, 0)) : 0;
    float result = current;
    if (temporal > 0 && AoHistory())
    {
        float3 V = ViewPosAt((pc + 0.5) / fullSize, dc);
        float4 prevClip = mul(float4(V.xy, -V.z, 1), reproj);
        float2 prevUv = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5;
        if (prevClip.w > 0 && all(prevUv >= 0) && all(prevUv <= 1))
        {
            float2 prev = historyTex.SampleLevel(pointClamp, prevUv, 0).xy;
            if (abs(prev.y - prevClip.w) < 0.04 * prevClip.w + 0.05)
                result = lerp(current, clamp(prev.x, current - 1.5 * sigma - 0.03, current + 1.5 * sigma + 0.03), 0.9);
        }
    }
    outTex[id.xy] = float4(result, zc, 0, 0);
}

// On the full-size GI (u0, read and written): the occlusion of the half-size texels around the pixel (bilinear times
// depth likeness), times the strength, as the engine's SSAO was: rgb (1 - ao), share 1 - (1 - share)(1 - ao), the
// flags (a = share + 2 * flags) kept. t0 depth, t1 the accumulated occlusion (x).
[numthreads(8, 8, 1)]
void gtaoApply(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float d = depthTex.Load(int3(id.xy, 0));
    if (d < kWeaponDepth || d >= 0.99999 || (id.x == 0 && id.y == 0)) // the top-left texel holds the shaders' control
        return;
    float z = depthParams.y / (d + depthParams.x);
    int2 aoSize = int2((fullSize + 1) / 2);
    int2 lim = int2(fullSize) - 1;
    float2 tc = (id.xy + 0.5) * 0.5 - 0.5;
    int2 t0 = int2(floor(tc));
    float2 f = tc - t0;
    float sum = 0, weights = 0;
    [unroll] for (int j = 0; j < 4; j++)
    {
        int2 o = int2(j & 1, j >> 1);
        int2 t = clamp(t0 + o, 0, aoSize - 1);
        float bil = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
        float w = bil * (exp(-abs(ZAt(min(t * 2, lim)) - z) / (0.02 * z + 0.05)) + 1e-3);
        sum += srcTex.Load(int3(t, 0)).x * w;
        weights += w;
    }
    float ao = saturate((AoScale() == 1 ? srcTex.Load(int3(id.xy, 0)).x : sum / max(weights, 1e-5)) * motionParams.y);
    float4 g = outTex[id.xy];
    float flags = floor(g.a * 0.5);
    float share = g.a - 2 * flags;
    // Leaves (t2, the foliage mask 1): the GI's occlusion and bounce times CRYMT_GI_FOLIAGEGI, the share kept under 0.9
    // (the ambient floor). Grass (the mask 0.6): times CRYMT_GI_GRASSGI and no GTAO - the engine's SSAO stays on it (its
    // blend with the terrain's colour takes only that; with GTAO in its place and the leaves' 1.5 it came out flat and too
    // bright, the user). motionParams.z for this pass: the leaves' factor + 4 x floor(the grass's x 100).
    float mask = historyTex.Load(int3(id.xy, 0)).x;
    if (mask > 0.5)
    {
        float packed = floor(motionParams.z / 4);
        float k = mask < 0.8 ? packed / 100 : motionParams.z - packed * 4;
        if (mask < 0.8)
            ao = 0;
        g.rgb *= k;
        share = min(share * k, max(share, 0.9));
    }
    outTex[id.xy] = float4(g.rgb * (1 - ao), 1 - (1 - share) * (1 - ao) + 2 * flags);
}

// The sun's visibility from a point (the particles' volumes): a ray towards it through the finest level that holds each
// step, from half a metre off, steps of a cell of that level or 8% of the distance, up to 48 m; 1 open .. 0 covered.
float SunVisible(float3 O, float3 dir)
{
    float covered = 0, d = 0.5;
    [loop] for (int i = 0; i < 64 && covered < 0.95 && d < 48; i++)
    {
        float3 X = O + dir * d;
        int level = 0;
        [loop] while (level < 3 && !InGrid(level, X, 0))
            level++;
        if (!InGrid(level, X, 0))
            break;
        covered += (1 - covered) * SampleGrid(level, X, 0).a;
        d += max(voxParams.x * (1 << level), d * 0.08);
    }
    return 1 - covered;
}

// The particles' occlusion volumes (Device::RunGi, for the patched Particles.cfi): reproj[0] = the coarse cell size (m),
// cells x, y, z; reproj[1] = slices across, the fine cell size; reproj[2] = towards the sun, w 1 when there is one. Two
// volumes side by side in a 2D atlas (u0, R32G32F; fine left, coarse right), each the cells around the camera (the first
// floor(eye / cell) - dims / 2) as z-slices tiled across: per cell x the share of ambient a blended particle there loses
// - what a surface facing up would take from the voxels: five cones up (60 degrees), the indoor share, Strengthen - and
// y the sun's visibility (SunVisible; -1 without a sun), z the indoor share; below the shares' row (one.y) the tints
// (rows one.y + 1 on: the hue of the cones' light, part white); reproj[3] = the outdoor part of the share, its cap, the
// tint's part. Row one.y: texels 0-3 the fine volume's first cell
// relative to the camera (metres) and cell size, 4-7 the coarse one's. One volume a frame, by turns (each keeps its place
// with its cells, so particles read it right a frame later; 0.79 ms a frame for both, the sun's rays most of it).
[numthreads(8, 8, 1)]
void pvolume(uint3 id : SV_DispatchThreadID)
{
    int3 dims = int3(reproj[0].yzw);
    int tx = (int)reproj[1].x;
    int2 one = int2(dims.x * tx, dims.y * ((dims.z + tx - 1) / tx));
    if (id.x >= (uint)one.x || id.y >= (uint)one.y)
        return;
    int cascade = (int)reproj[1].z; // one volume a frame, by turns (reproj[1].z: 0 fine, 1 coarse)
    int2 lid = int2(id.xy);
    id.x += cascade * one.x;
    float cell = cascade == 0 ? reproj[1].y : reproj[0].x;
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    float3 first = floor(eye / cell) - float3(dims / 2);
    if (lid.y == 0 && lid.x < 4)
    {
        float4 place = float4(first * cell - eye, cell);
        outTex[int2(cascade * 4 + lid.x, one.y)] = place[lid.x];
    }
    int2 tile = lid / dims.xy;
    int z = tile.x + tile.y * tx;
    if (z >= dims.z)
    {
        outTex[id.xy] = 0;
        outTex[int2(id.x, id.y + one.y + 1)] = 1;
        return;
    }
    float3 P = (first + float3(lid - tile * dims.xy, z) + 0.5) * cell;
    float3 up = float3(0, 0, 1), T = float3(1, 0, 0), B = float3(0, 1, 0);
    float start = voxParams.x * 2;
    float4 cone = ConeTrace(P, up, 0.577, start, voxParams.z, false, false, P, up, 0.5);
    float occ = cone.a;
    float3 light = cone.rgb;
    [unroll] for (int c = 0; c < 4; c++)
    {
        float a = (c + 0.5) * 1.5707963;
        float3 dir = normalize(up * 0.5 + (T * cos(a) + B * sin(a)) * 0.866);
        cone = ConeTrace(P, dir, 0.577, start, voxParams.z, false, false, P, up, 0.5 + c * 0.25);
        occ += cone.a * 0.5;
        light += cone.rgb * 0.5;
    }
    // The tint: the hue of the light the cones bring (luminance 1), reproj[3].z of it, the rest white; white where they
    // bring almost none (night, outside the grid).
    float lum = dot(light, float3(0.2126, 0.7152, 0.0722));
    float3 hue = lum > 1e-3 ? clamp(light / lum, 0, 3) : 1;
    outTex[int2(id.x, id.y + one.y + 1)] = float4(lerp(1, hue, reproj[3].z), 0);
    float roof = VisInside(P);
    if (roof < 1 && occRange.y != motionParams.w)
        roof = max(roof, RoofCover(P, up, 0.5));
    // Outdoors a part of the occlusion (reproj[3].x; light haze and dust vanished against the background), at most
    // reproj[3].y; z the indoor share (the particles take the voxels' sun indoors, the game's outdoors).
    float share = Strengthen(saturate(occ / 3 * voxParams2.x), roof);
    share = min(share * lerp(reproj[3].x, 1, roof), reproj[3].y);
    float sun = reproj[2].w > 0.5 ? SunVisible(P, normalize(reproj[2].xyz)) : -1;
    outTex[id.xy] = float4(share, sun, roof, 0);
}

// Exposure for DLSS (Device::RunExposure, one group): the average log2 luminance of the scene's HDR colour (t1) over a
// 64x32 grid, exposure = radius (the key) / that, clamped to [occlusion, maxDistance] and eased toward per frame by
// bounce (temporal 1: at once); u1 the 1x1 R32F exposure, read and written. DLSS's own auto exposure left night shade
// almost black to it: its reconstruction then turned the noise there into white single-pixel sparkles.
groupshared float gsLum[256];
[numthreads(16, 16, 1)]
void exposure(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    float sum = 0;
    [unroll] for (int k = 0; k < 8; k++)
    {
        float2 uv = (float2(id.x + (k & 3) * 16, id.y + (k >> 2) * 16) + 0.5) / float2(64, 32);
        float lum = dot(srcTex.SampleLevel(pointClamp, uv, 0).rgb, float3(0.2126, 0.7152, 0.0722));
        lum = lum >= 0 ? min(lum, 1e4) : 0; // NaN, negative
        sum += log2(max(lum, 1e-5));
    }
    gsLum[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint n = 128; n > 0; n >>= 1)
    {
        if (gi < n)
            gsLum[gi] += gsLum[gi + n];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        float target = clamp(radius / exp2(gsLum[0] / 2048), occlusion, maxDistance);
        float previous = distOut[uint2(0, 0)];
        distOut[uint2(0, 0)] = previous > 0 && temporal < 1 ? exp2(lerp(log2(previous), log2(target), bounce)) : target;
    }
}
)";

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
constexpr UINT kGiVoxTable = 9 * kVoxLevels + 4;
constexpr UINT kProbeCascades = 2; // GI v2's probe cascades (kGiShader's kProbeCascades: 0.5 m and 1 m apart)
constexpr UINT kVoxTable = 23; // descriptors per table: t0..t2, u0..u19 (u6/u7 the light direction, u8..u19 the six
                               // directions: CRYMT_GI_DIRLIGHT)
const char* const kVoxShader = R"(
cbuffer VoxConstants : register(b0)
{
    row_major float4x4 worldToView;     // the engine's view space (x, y, -distance)
    row_major float4x4 worldToPrevClip; // the previous frame's clip space (where its colour is)
    float4 toWorldX; float4 toWorldY; float4 toWorldZ; // view (x, y, z, 1) dotted with these: world x, y, z
    float2 fullSize; float2 invProj;
    float2 depthParams; float voxelSize; float mode;
    float4 gridMin;     // xyz the grid's first cell, w cells per axis
    float4 prevGridMin; // xyz the last update's
};
Texture2D<float4> depthTex : register(t0);  // update: scene depth (r); view: the info panel
Texture2D<float4> colourTex : register(t1);
Texture2D<float4> sceneTex : register(t2);
Texture2D<float> foliageTex : register(t2); // update: the z-prepass's foliage mask (1 on alpha-tested surfaces), null without
RWTexture3D<float4> voxels : register(u0);
RWTexture2D<float4> viewOut : register(u1);
RWTexture3D<uint> occupancy : register(u1); // update: the voxelized geometry's frame stamps (levels 0-2)
RWTexture3D<uint> seen : register(u3);      // update, light: bit 0 the screen gave the cell its colour; bits 1-7
                                            // how many updates in a row it showed the cell empty (kClearAfter)
// Kept geometry goes when the screen showed its cell empty this many updates in a row with no new stamp: one depth sample
// a cell and update flips between a thin part, the DLAA jitter and LOD changes, and the cells came and went.
static const uint kClearAfter = 4; // updates: 4, 8, 16 frames at levels 0-2 (periods 1, 2, 4)
RWTexture3D<unorm float4> albedo : register(u4); // light: the material colour the geometry pass stored (a 1 when set)
RWTexture3D<float4> voxelsDst : register(u2);
RWTexture3D<float4> voxels1 : register(u3); // view: grid levels 1..3 (their first cells in worldToPrevClip rows 1..3)
RWTexture3D<float4> voxels2 : register(u4);
RWTexture3D<float4> voxels3 : register(u5);
cbuffer VoxLamps : register(b1) // light: the lamps (DlssSceneInfo::lamps), world metres
{
    float4 lampPos[8];    // xyz, radius
    float4 lampColour[8]; // rgb (HDR, times CRYMT_GI_LAMPS), the cosine of a projector's half angle (-2: a point light)
    float4 lampDir[8];    // a projector's direction
    float4 lampCount;
};
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);
RWTexture3D<unorm float4> dirSrc : register(u3); // downsample: the per-axis opacity, the mip below (grid mip 2 and up)
RWTexture3D<unorm float4> dirDst : register(u4); // and the one written
RWTexture3D<unorm float> roofOut : register(u5);  // roof: cover above per cell of level 0 at half its resolution
// The cells' light direction (CRYMT_GI_DIRLIGHT 1, voxFlags bit 0): g, |g| <= 1; the light going out towards w is the
// colour times 1 + g.w (a thin wall lit outside: the colour the mean of both sides, g pointing out). Downsample: u6 the
// mip below, u7 the mip written.
RWTexture3D<snorm float4> l1Grid : register(u6);
RWTexture3D<snorm float4> l1Dst : register(u7);
cbuffer VoxFlags : register(b2) { uint voxFlags; };
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
// Six directions (CRYMT_GI_DIRLIGHT 2, voxFlags bit 1): the light going out towards +X, -X, +Y, -Y, +Z, -Z (kSixAxis).
// u8..u13 the level (downsample: the mip below), u14..u19 the downsample's mip.
RWTexture3D<float3> six0 : register(u8);
RWTexture3D<float3> six1 : register(u9);
RWTexture3D<float3> six2 : register(u10);
RWTexture3D<float3> six3 : register(u11);
RWTexture3D<float3> six4 : register(u12);
RWTexture3D<float3> six5 : register(u13);
RWTexture3D<float3> sixDst0 : register(u14);
RWTexture3D<float3> sixDst1 : register(u15);
RWTexture3D<float3> sixDst2 : register(u16);
RWTexture3D<float3> sixDst3 : register(u17);
RWTexture3D<float3> sixDst4 : register(u18);
RWTexture3D<float3> sixDst5 : register(u19);
static const float3 kSixAxis[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
float3 SixLoad(int f, uint3 p)
{
    [forcecase] switch (f)
    {
    case 0: return six0[p];
    case 1: return six1[p];
    case 2: return six2[p];
    case 3: return six3[p];
    case 4: return six4[p];
    default: return six5[p];
    }
}
void SixStore(int f, uint3 p, float3 c)
{
    [forcecase] switch (f)
    {
    case 0: six0[p] = c; break;
    case 1: six1[p] = c; break;
    case 2: six2[p] = c; break;
    case 3: six3[p] = c; break;
    case 4: six4[p] = c; break;
    default: six5[p] = c; break;
    }
}
void SixDstStore(int f, uint3 p, float3 c)
{
    [forcecase] switch (f)
    {
    case 0: sixDst0[p] = c; break;
    case 1: sixDst1[p] = c; break;
    case 2: sixDst2[p] = c; break;
    case 3: sixDst3[p] = c; break;
    case 4: sixDst4[p] = c; break;
    default: sixDst5[p] = c; break;
    }
}

static const float kWeaponDepth = 0.1;
static const float kColourTrust = 48; // metres: screen colour taken at full weight up to here (update)
bool Finite3(float3 v) { return all((asuint(v) & 0x7f800000) != 0x7f800000); }
int3 Wrap(int3 cell) { int n = (int)gridMin.w; return (cell % n + n) % n; }

// The visible surface at a pixel in the view space of worldToView (x, y, -distance); far plane: very far.
float3 SurfaceAt(float2 uv)
{
    float d = depthTex.SampleLevel(pointClamp, uv, 0).r;
    float z = d >= 0.99999 ? 1e9 : depthParams.y / (d + depthParams.x);
    return float3(float2(uv.x * 2 - 1, 1 - uv.y * 2) * invProj * z, -z);
}

// Of two differences to the neighbours, the one across less depth (not over an edge).
float3 Nearer(float3 a, float3 b) { return abs(a.z) < abs(b.z) ? a : b; }
float3 Tone(float3 c) { return c / (1 + c); }

// The previous frame's colour at puv (a cell's reprojection) belongs to the first-person view (weapon, hands: depth under
// kWeaponDepth) where the screen shows it now, the bilinear footprint and a pixel and a half around: it stays in place
// while the world turns behind it, so cells coming out from behind it took the hands' colour into the GI's bounce (the
// user, 2026-11-13: "the hands shade other objects in the background").
bool WeaponAt(float2 puv)
{
    float2 px = 1.5 / fullSize;
    float d = depthTex.SampleLevel(pointClamp, puv, 0).r;
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(-px.x, -px.y), 0).r);
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(px.x, -px.y), 0).r);
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(-px.x, px.y), 0).r);
    d = min(d, depthTex.SampleLevel(pointClamp, puv + float2(px.x, px.y), 0).r);
    return d < kWeaponDepth;
}

// CRYMT_GI_DIRLIGHT 1: the screen's colour c is the light the cell sends out towards the camera's side of its surface
// (nView, view space): that side moves to it, the other side keeps its light (a cell seen for the first time: dark,
// unknown); the colour becomes the two sides' mean and g their difference along the normal over their sum (the rest of g
// fades). A wall seen from inside keeps the sun its outer side got.
void ScreenL1(uint3 id, inout float4 v, float3 nView, float3 c, float trust)
{
    float3 n = normalize(float3(dot(nView, toWorldX.xyz), dot(nView, toWorldY.xyz), dot(nView, toWorldZ.xyz)));
    float3 g = l1Grid[id].xyz;
    float gn = dot(g, n);
    bool fresh = v.a <= 0.01;
    float3 front = fresh ? c * trust : lerp(v.rgb * (1 + gn), c, 0.25 * trust);
    float3 back = fresh ? 0 : v.rgb * max(1 - gn, 0);
    v.rgb = 0.5 * (front + back);
    float lf = Luma(front), lb = Luma(back);
    float3 gNew = n * ((lf - lb) / max(lf + lb, 1e-4)) + (g - n * gn) * 0.75;
    float len = length(gNew);
    l1Grid[id] = float4(len > 1 ? gNew / len : gNew, 0);
}

// CRYMT_GI_DIRLIGHT 2: the screen's colour goes into the faces turned to the camera's side of the surface (by how much
// they face it), the others keep theirs (a cell seen for the first time: dark).
void ScreenSix(uint3 id, float3 nView, float3 c, float trust, bool fresh)
{
    float3 n = normalize(float3(dot(nView, toWorldX.xyz), dot(nView, toWorldY.xyz), dot(nView, toWorldZ.xyz)));
    [unroll] for (int f = 0; f < 6; f++)
    {
        float w = dot(n, kSixAxis[f]);
        if (fresh)
            SixStore(f, id, w > 0.2 ? c * trust : 0);
        else if (w > 0)
            SixStore(f, id, lerp(SixLoad(f, id), c, 0.25 * trust * saturate(w * 2)));
    }
}

[numthreads(4, 4, 4)]
void update(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w;
    if (any(id >= (uint)n))
        return;
    int3 g = (int3)gridMin.xyz, pg = (int3)prevGridMin.xyz;
    int3 cell = g + Wrap(int3(id) - g); // the cell of the current grid stored here
    float4 v = voxels[id];
    bool entered = any(cell < pg) || any(cell >= pg + n);
    if (entered)
    {
        v = 0; // entered the grid now: nothing known
        if (voxFlags & 1)
            l1Grid[id] = 0;
        if (voxFlags & 2)
            [unroll] for (int f0 = 0; f0 < 6; f0++)
                SixStore(f0, id, 0);
    }
    bool track = prevGridMin.w > 0; // the geometry's levels keep which cells the screen coloured
    if (track && entered)
    {
        seen[id] = 0;
        // The geometry stamps stored here belong to the cell that left on the far side (toroidal addressing): kept only
        // if this frame's voxelization (stamp: mode) already wrote the new cell, else from the next frame on they stood
        // as a wall of cells at the grid's edge for the stamps' lifetime, each time the grid moved (the user: a wall of
        // voxels at the edge of their reach, now and then; a cube like a sky dome in the Num2 view).
        if ((occupancy[id] >> 8) != (uint)abs(mode))
            occupancy[id] = 0;
    }
    // The z-prepass draws voxelized into this level (RunVoxelGeometry): the stamp of the last frame that drew into the
    // cell; a recent one makes it solid, also where the screen never showed the surface (undersides, backs).
    // Kept (mode < 0, CRYMT_GI_GEOMPERSIST): any stamp, until the screen shows the cell empty or it leaves the grid;
    // else the last three voxelizations only (geometry the engine stopped drawing, a room seen through a portal once
    // the camera left it, went from the grid: the GI jumped when entering).
    uint geometry = prevGridMin.w > 0 && all(cell >= pg) && all(cell < pg + n) ? occupancy[id] : 0;
    // A stamp at all (without voxel geometry prevGridMin.w is 0 and every empty cell passed the age test: all were taken
    // for foliage and eased to opacity 0 - the user: Num3 off left no occlusion at all).
    bool stamped = geometry != 0 && ((geometry >> 8) >= (uint)prevGridMin.w || (mode < 0 && (geometry >> 8) > 0));
    float stampA = (geometry & 255) / 255.0; // solid 1, foliage CRYMT_GI_FOLIAGE (drawn without its alpha test)
    // Foliage cells (a partly open stamp: the voxelized leaves, by their shape) take their opacity from the stamps alone,
    // eased: the screen made them solid where it saw a leaf and halved them where it saw through a gap between leaves, so
    // they changed every update as the leaves swayed and the camera moved, and the GI under trees flickered (the user:
    // only turning the GI off stopped it). The screen still gives them their colour.
    bool foliage = stamped && stampA < 0.98;
    // CRYMT_GI_OCCUPANCY=geometry (voxels page Num0): the opacity from the stamps alone, the screen gives the colour; what
    // the screen saw is not made solid or open (it depends on where the camera is: cells came and went as it moved).
    bool geomOnly = track && frac(abs(mode)) > 0.25;
    float3 centre = (cell + 0.5) * voxelSize;
    float3 V = mul(float4(centre, 1), worldToView).xyz;
    float dist = -V.z;
    // The engine cube's stamps (RunVoxelGeometry: opacity byte 253, frame 1, so never newer than a z-prepass stamp) fill
    // what the camera does not see; in its view the z-prepass and the screen decide (the cube may draw another level of
    // detail: those cells would come back at every turn of the cube and go again).
    if (stamped && (geometry & 255) == 253 && dist > 0.3 && all(abs(V.xy / (dist * invProj)) <= 1))
        stamped = false;
    if (dist > 0.3)
    {
        float2 ndc = V.xy / (dist * invProj);
        float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (all(uv >= 0) && all(uv <= 1))
        {
            float d = depthTex.SampleLevel(pointClamp, uv, 0).r;
            if (d >= kWeaponDepth)
            {
                float surface = d >= 0.99999 ? 1e9 : depthParams.y / (d + depthParams.x);
                float tolerance = voxelSize * 0.87;
                // Where the cell is against the surface: along the ray it lay far in front of a surface seen at a
                // grazing angle (a cell on a road a third of a cell above the asphalt, seen at 10 degrees: nearly two
                // cells), and was carved out: stripes of holes across roads by the phase of their steps, dark spots,
                // cells that came and went as the camera moved (the user). Near the surface it is measured along the
                // surface's normal (from the depth of the neighbouring pixels); far in front along the ray as before.
                float side = dist - surface; // < 0 in front of the surface, > 0 behind it
                float3 surfN = 0;            // the surface's normal there (view space, towards the camera), if found
                if (d < 0.99999 && side > -(tolerance + 24 * voxelSize) && side < tolerance + 3 * voxelSize)
                {
                    float2 px = 1 / fullSize;
                    float3 P0 = SurfaceAt(uv);
                    float3 tx = Nearer(SurfaceAt(uv + float2(px.x, 0)) - P0, P0 - SurfaceAt(uv - float2(px.x, 0)));
                    float3 ty = Nearer(SurfaceAt(uv + float2(0, px.y)) - P0, P0 - SurfaceAt(uv - float2(0, px.y)));
                    float3 N = cross(tx, ty);
                    if (dot(N, N) > 1e-12)
                    {
                        N = normalize(N);
                        if (dot(N, P0) > 0)
                            N = -N; // towards the camera
                        side = -dot(V - P0, N);
                        surfN = N;
                    }
                }
                if (side < -tolerance)
                {
                    // Seen through: halved, unless the voxelized geometry says something is there (thin things - a
                    // fence, a pole, branches - the screen sees past: halved here, set again by the next voxelization,
                    // they came and went as the camera moved). Stamps of things that went are cleared below.
                    if (!foliage && !stamped && !geomOnly)
                        v.a *= 0.5;
                    if (track)
                    {
                        // Kept geometry (mode < 0) that the screen shows empty goes (a moved object, a door) after
                        // kClearAfter such updates in a row; a recent stamp (drawn within the window) restarts the
                        // count. Not foliage: the screen sees through the gaps between leaves from most places, and the
                        // kept leaves went and came back as the camera moved (the crowns flickered while walking).
                        const bool fresh = (occupancy[id] >> 8) >= (uint)prevGridMin.w;
                        const uint empty = fresh ? 0 : min((seen[id] >> 1) + 1, 127u);
                        seen[id] = empty << 1;
                        if (mode < 0 && !foliage && !fresh && empty >= kClearAfter)
                            occupancy[id] = 0;
                    }
                }
                else if (side < tolerance)
                {
                    float4 pc = mul(float4(centre, 1), worldToPrevClip);
                    float2 puv = float2(pc.x, -pc.y) / pc.w * 0.5 + 0.5;
                    float3 c = colourTex.SampleLevel(linearClamp, puv, 0).rgb;
                    if (pc.w > 0 && all(puv >= 0) && all(puv <= 1) && Finite3(c) && !WeaponAt(puv))
                    {
                        c = clamp(c, 0, 8);
                        // Colour seen from afar is mostly fog and haze (at night lighter than the surfaces: the
                        // coarse levels lit a band at their edge until the camera came near): trusted fully up to
                        // kColourTrust metres, less beyond; a first sight from afar is dimmed likewise.
                        float trust = saturate(kColourTrust / dist);
                        if ((voxFlags & 2) && dot(surfN, surfN) > 0)
                            ScreenSix(id, surfN, c, trust, v.a <= 0.01);
                        if ((voxFlags & 1) && dot(surfN, surfN) > 0)
                            ScreenL1(id, v, surfN, c, trust);
                        else
                            v.rgb = v.a > 0.01 ? lerp(v.rgb, c, 0.25 * trust) : c * trust;
                        // A leaf on screen (the foliage mask) is no wall: half open at most, as the stamps have it.
                        if (!foliage && !geomOnly)
                            v.a = lerp(v.a, foliageTex.SampleLevel(pointClamp, uv, 0) >= 0.5 ? 0.5 : 1, 0.5);
                        if (track)
                            seen[id] = 1;
                    }
                }
                else if (side < tolerance + 2 * voxelSize && !foliage && !geomOnly && foliageTex.SampleLevel(pointClamp, uv, 0) < 0.5)
                {
                    // Just behind a solid visible surface: the inside of the ground, a wall, a rock, filled with the
                    // surface's colour. The screen gave a surface one layer of cells, in steps along a slope with
                    // nothing between them (the user: holes across a road in the Num2 view, dark spots that went with
                    // the camera); behind foliage stays as it was (leaves are thin).
                    float4 pc = mul(float4(centre, 1), worldToPrevClip);
                    float2 puv = float2(pc.x, -pc.y) / pc.w * 0.5 + 0.5;
                    float3 c = colourTex.SampleLevel(linearClamp, puv, 0).rgb;
                    if (pc.w > 0 && all(puv >= 0) && all(puv <= 1) && Finite3(c) && !WeaponAt(puv))
                    {
                        float trust = saturate(kColourTrust / dist);
                        v.rgb = v.a > 0.01 ? lerp(v.rgb, clamp(c, 0, 8), 0.25 * trust) : clamp(c, 0, 8) * trust;
                        v.a = lerp(v.a, 1, 0.5);
                        if (track)
                            seen[id] = 1;
                    }
                }
            }
        }
    }
    if (geomOnly)
        v.a = stamped ? (foliage ? lerp(v.a, stampA, 0.3) : stampA) : v.a * 0.5; // no stamp: gone within a few updates
    else if (stamped)
        v.a = foliage ? lerp(v.a, stampA, 0.3) : max(v.a, stampA);
    voxels[id] = v;
}

// The lamps' light at a cell (light pass): falloff (1 - d^2 / r^2)^2, a projector's cone with a soft edge, on the side
// the neighbours say faces the lamp, where a march through this level towards it (stopping a cell and a half short of
// it: the fixture) finds no cover.
float3 LampLight(int3 cell, float3 N, bool flat, int3 g, int n)
{
    float3 sum = 0;
    float3 P = (cell + 0.5) * voxelSize;
    [loop] for (int k = 0; k < (int)lampCount.x; k++)
    {
        float3 D = lampPos[k].xyz - P;
        float r = lampPos[k].w, dd = dot(D, D);
        if (dd >= r * r)
            continue;
        float d = sqrt(dd);
        float3 Ld = D / max(d, 1e-3);
        float att = saturate(1 - dd / (r * r));
        float cosHalf = lampColour[k].w;
        float cone = cosHalf > -1 ? saturate((dot(-Ld, lampDir[k].xyz) - cosHalf) / max(1 - cosHalf, 1e-3) * 4) : 1;
        float face = flat ? 0.5 : saturate(dot(N, Ld));
        float lit = att * att * cone * face;
        if (lit <= 0)
            continue;
        float cells = d / voxelSize, step = max(1, (cells - 3) / 16), cover = 0;
        float3 q = cell + 0.5 + Ld * 1.5;
        [loop] for (float t = 1.5; t < cells - 1.5 && cover < 0.95; t += step)
        {
            int3 c = (int3)floor(q);
            if (any(c < g) || any(c >= g + n))
                break;
            cover += (1 - cover) * voxels[Wrap(c)].a;
            q += Ld * step;
        }
        sum += lampColour[k].rgb * lit * (1 - cover);
    }
    return sum;
}

// Light for cells the voxelized geometry made solid but the screen never coloured (seen 0), at every update of the level
// (the time of day moves): the sun's colour (row 1 of worldToPrevClip, which this pass does not need) times an albedo
// (row 2 x), on the side the opacity of the six neighbours says faces the sun (row 0 xyz, towards it), where a march
// through this level towards it finds no cover; plus a sky fill (row 2 y, a share of the sun's colour, more facing up)
// where a march straight up finds open sky (none under a roof); plus the lamps (b1). Row 2 z: also cells the screen
// coloured (CRYMT_GI_LAMPTEST). A quarter of the way to it per update: lamps culled, broken or moving fade instead of
// switching the cells' light from one update to the next.
[numthreads(4, 4, 4)]
void light(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w;
    if (any(id >= (uint)n))
        return;
    float4 v = voxels[id];
    if (v.a < 0.5 || ((seen[id] & 1) != 0 && worldToPrevClip[2].z == 0))
        return;
    float3 L = worldToPrevClip[0].xyz;
    int3 g = (int3)gridMin.xyz;
    int3 cell = g + Wrap(int3(id) - g);
    float3 grad = float3(voxels[Wrap(cell - int3(1, 0, 0))].a - voxels[Wrap(cell + int3(1, 0, 0))].a,
                         voxels[Wrap(cell - int3(0, 1, 0))].a - voxels[Wrap(cell + int3(0, 1, 0))].a,
                         voxels[Wrap(cell - int3(0, 0, 1))].a - voxels[Wrap(cell + int3(0, 0, 1))].a);
    bool flat = dot(grad, grad) <= 0.01;
    float3 N = flat ? 0 : normalize(grad);
    float facing = flat ? 0.5 : saturate(dot(N, L));
    // The material's colour where the geometry pass sampled one (scaled so an average texture of 0.5 gives the albedo
    // setting), else the setting itself (grey).
    float4 material = albedo[id];
    float3 surface = worldToPrevClip[2].x * (material.a > 0 ? material.rgb / 0.5 : 1);
    float3 albedoSun = worldToPrevClip[1].rgb * surface;
    float3 sky = albedoSun * worldToPrevClip[2].y * (flat ? 0.75 : 0.5 + 0.5 * N.z);
    float cover = 0;
    float3 p = cell + 0.5 + L * 1.5;
    const int sunSteps = worldToPrevClip[2].w > 0 ? (int)worldToPrevClip[2].w : 24; // CRYMT_GI_SUNMARCH, cells of this level
    [loop] for (int i = 0; i < sunSteps && cover < 0.95 && facing > 0; i++)
    {
        int3 c = (int3)floor(p);
        if (any(c < g) || any(c >= g + n))
            break;
        cover += (1 - cover) * voxels[Wrap(c)].a;
        p += L;
    }
    float roof = 0;
    p = cell + 0.5 + float3(0, 0, 1.5);
    [loop] for (int j = 0; j < 24 && roof < 0.95; j++)
    {
        int3 c = (int3)floor(p);
        if (any(c < g) || any(c >= g + n))
            break;
        roof += (1 - roof) * voxels[Wrap(c)].a;
        p.z += 1;
    }
    const float3 sunPart = albedoSun * facing * (1 - cover);
    const float3 rest = sky * (1 - roof) + surface * LampLight(cell, N, flat, g, n);
    if (voxFlags & 1)
    {
        // CRYMT_GI_DIRLIGHT 1: the sun on the side towards it only (a thin wall: its outer face), the sky and the lamps on
        // both: the cell sends rest + sun out towards the sun's side and rest away from it.
        const float3 target = rest + 0.5 * sunPart;
        const float lt = Luma(target);
        const float3 side = flat ? L : N * (dot(N, L) >= 0 ? 1 : -1);
        const float3 dirT = lt > 1e-5 ? side * (0.5 * Luma(sunPart) / lt) : 0;
        v.rgb = lerp(v.rgb, target, 0.25);
        l1Grid[id] = float4(lerp(l1Grid[id].xyz, dirT, 0.25), 0);
        if (voxFlags & 2) // six directions: the sun into the faces turned to the lit side, the rest into all
            [unroll] for (int f = 0; f < 6; f++)
                SixStore(f, id, lerp(SixLoad(f, id), rest + sunPart * saturate(2 * dot(side, kSixAxis[f])), 0.25));
    }
    else
        v.rgb = lerp(v.rgb, sunPart + rest, 0.25);
    voxels[id] = v;
}

// The engine cube's face just drawn (CRYMT_GI_CUBEINJECT, RunCubeInject): every cell of this level that the face saw as
// a surface, outside the camera's view, gets the cube's stamp (frame 1, opacity byte 253: the update counts it only
// outside the view; never newer than a geometry stamp) and the face's lit colour, and is marked coloured (seen bit 0:
// the light pass leaves it). worldToPrevClip: the face's view-projection (clip.w = the distance along its axis);
// toWorldX: its depth (distance = y / (d + x)) and size; t0 its depth, t1 its HDR colour; prevGridMin: the level's cells.
[numthreads(4, 4, 4)]
void inject(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w;
    if (any(id >= (uint)n))
        return;
    int3 g = (int3)prevGridMin.xyz;
    int3 cell = g + Wrap(int3(id) - g);
    float3 centre = (cell + 0.5) * voxelSize;
    float4 clip = mul(float4(centre, 1), worldToPrevClip);
    if (clip.w < 1.5) // the face's near plane (1.5 m with entities)
        return;
    float2 ndc = clip.xy / clip.w;
    if (any(abs(ndc) > 1))
        return;
    // In the camera's view the z-prepass and the screen decide.
    float3 V = mul(float4(centre, 1), worldToView).xyz;
    if (-V.z > 0.3 && all(abs(V.xy / (-V.z * invProj)) <= 1))
        return;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float tolerance = voxelSize * 0.87;
    // The face sees well past the cell (every texel of the 3x3 around it beyond it): a cube stamp there is stale (an
    // entity that moved, a door) and goes; the geometry's stamps stay (the update and the screen decide on those).
    float nearest = 1e9;
    [unroll] for (int j = -1; j <= 1; j++)
        [unroll] for (int i = -1; i <= 1; i++)
        {
            float dn = depthTex.SampleLevel(pointClamp, uv + float2(i, j) / toWorldX.zw, 0).r;
            nearest = min(nearest, dn >= 0.99999 ? 1e9 : toWorldX.y / (dn + toWorldX.x));
        }
    if (clip.w < nearest - 2 * tolerance)
    {
        if (occupancy[id] == 256u + 253u)
        {
            occupancy[id] = 0;
            float4 gone = voxels[id];
            gone.a = 0;
            voxels[id] = gone;
        }
        return;
    }
    float d = depthTex.SampleLevel(pointClamp, uv, 0).r;
    if (d >= 0.99999) // the sky
        return;
    float surface = toWorldX.y / (d + toWorldX.x);
    if (abs(clip.w - surface) > tolerance)
        return;
    float3 c = colourTex.SampleLevel(pointClamp, uv, 0).rgb;
    if (!Finite3(c))
        return;
    uint old;
    InterlockedMax(occupancy[id], 256u + 253u, old);
    float4 v = voxels[id];
    float trust = saturate(kColourTrust / clip.w); // far: mostly fog and haze (as the screen's colour)
    c = clamp(c, 0, 8);
    v.rgb = v.a > 0.01 ? lerp(v.rgb, c, 0.5 * trust) : c * trust;
    v.a = max(v.a, 253.0 / 255.0);
    voxels[id] = v;
    seen[id] = seen[id] | 1u;
}

// Cover above per cell of level 0 at half its resolution (roofOut, the GI's RoofAt): five rays up (straight and four
// leaning 25 degrees) from the centre of the 2x2x2 block through level 0's cells within 32 cells (8 m), each covered
// by the opacity it meets (1 - the product of 1 - a); the mean. An eighth of the texture a frame (mode: the slab of z).
// (The geometry pass's opacity byte, which would tell leaves from roofs, read as 0 here: to look into.)
[numthreads(4, 4, 4)]
void roof(uint3 id : SV_DispatchThreadID)
{
    int n = (int)gridMin.w, half = n / 2;
    uint3 t = uint3(id.xy, id.z + (uint)mode * (uint)(half / 8));
    if (any(t >= (uint)half))
        return;
    int3 g = (int3)gridMin.xyz;
    int3 cell = g + Wrap(int3(t) * 2 - g); // the block's first cell in the grid now
    static const float3 kRays[5] = {float3(0, 0, 1), float3(0.42, 0, 0.91), float3(-0.42, 0, 0.91), float3(0, 0.42, 0.91), float3(0, -0.42, 0.91)};
    float stopped = 0;
    [loop] for (int r = 0; r < 5; r++)
    {
        float3 p = cell + 1 + kRays[r] * 1.5;
        float covered = 0;
        [loop] for (int k = 0; k < 32 && covered < 0.95; k++)
        {
            int3 c = (int3)floor(p);
            if (any(c < g) || any(c >= g + n))
                break;
            covered += (1 - covered) * voxels[Wrap(c)].a;
            p += kRays[r];
        }
        stopped += covered;
    }
    roofOut[t] = stopped / 5;
}

// One mip of the grid from the one below (u0 the level below, u2 this level; mode = this level). Toroidal addressing
// carries over: the children of texel t are 2t + (0..1). Level 1 premultiplies level 0's colour by its opacity, the
// cones sample levels 1 and up (premultiplied colour, mean opacity).
[numthreads(4, 4, 4)]
void downsample(uint3 id : SV_DispatchThreadID)
{
    uint n = (uint)gridMin.w >> (uint)mode;
    if (any(id >= n))
        return;
    float4 sum = 0;
    float a[8];
    [unroll] for (int j = 0; j < 8; j++)
    {
        float4 v = voxels[id * 2 + uint3(j & 1, (j >> 1) & 1, j >> 2)];
        if (mode == 1)
            v.rgb *= v.a;
        sum += v;
        a[j] = v.a;
    }
    sum /= 8;
    // The opacity as how much of a ray the block stops (worldToView[0].w, CRYMT_GI_MIPOPACITY): along each axis the mean
    // over its four columns of 1 - (1 - a near) (1 - a far); 1 the mean of the three axes, 2 the largest, 3 the largest
    // where it says wall (a full layer: 1) and the mean where it says scattered (a lone cell 0.25: foliage, clutter stay
    // as thin as before), blended between 0.5 and 1; 4 per axis (below). 0: the plain mean, which thins a one-cell wall
    // to half per mip (the cones saw through walls and roofs a few metres on).
    float mipOpacity = worldToView[0].w;
    if (mipOpacity > 3)
    {
        // 4: the opacity per axis in its own texture (xyz; the cones weight it by their direction) and the plain mean
        // in the grid (the light's weight, the roof pass at mip 0, the views). From cells (grid mip 1) the material
        // opacities are independent: a column stops 1 - (1 - a) (1 - b); from coverages (mip 2 and up) a wall's halves
        // line up: the column stops what its more covered half does, so a floor or the ground stays as thin for a ray
        // along it as the mean says and a wall stops a ray across it.
        float3 d[8];
        [unroll] for (int j2 = 0; j2 < 8; j2++)
            d[j2] = mode == 1 ? a[j2].xxx : dirSrc[id * 2 + uint3(j2 & 1, (j2 >> 1) & 1, j2 >> 2)].xyz;
        float3 o = 0;
        [unroll] for (int k = 0; k < 4; k++)
        {
            int jx = (k & 1) * 2 + (k >> 1) * 4, jy = (k & 1) + (k >> 1) * 4, jz = k; // the columns' first cells
            if (mode == 1)
                o += 1 - (1 - float3(d[jx].x, d[jy].y, d[jz].z)) * (1 - float3(d[jx + 1].x, d[jy + 2].y, d[jz + 4].z));
            else
                o += max(float3(d[jx].x, d[jy].y, d[jz].z), float3(d[jx + 1].x, d[jy + 2].y, d[jz + 4].z));
        }
        dirDst[id] = float4(o / 4, 0);
    }
    else if (mipOpacity > 0)
    {
        float ax = 0, ay = 0, az = 0;
        [unroll] for (int k = 0; k < 4; k++)
        {
            int jx = (k & 1) * 2 + (k >> 1) * 4, jy = (k & 1) + (k >> 1) * 4, jz = k; // the columns' first cells
            ax += 1 - (1 - a[jx]) * (1 - a[jx + 1]);
            ay += 1 - (1 - a[jy]) * (1 - a[jy + 2]);
            az += 1 - (1 - a[jz]) * (1 - a[jz + 4]);
        }
        float most = max(ax, max(ay, az)) / 4;
        if (mipOpacity > 2)
            sum.a = lerp(sum.a, most, saturate(most * 2 - 1));
        else
            sum.a = mipOpacity > 1 ? most : (ax + ay + az) / 12;
    }
    voxelsDst[id] = sum;
    if (voxFlags & 1) // the light direction: the children's, weighted by their light (CRYMT_GI_DIRLIGHT 1)
    {
        float3 gs = 0;
        float ws = 0;
        [unroll] for (int j3 = 0; j3 < 8; j3++)
        {
            uint3 cid = id * 2 + uint3(j3 & 1, (j3 >> 1) & 1, j3 >> 2);
            float4 cv = voxels[cid];
            float w = Luma(cv.rgb) * (mode == 1 ? cv.a : 1);
            gs += l1Grid[cid].xyz * w;
            ws += w;
        }
        l1Dst[id] = float4(ws > 1e-6 ? gs / ws : 0, 0);
    }
    // The six directions: premultiplied means, as the colour. (An anisotropic front-to-back composite per face was tried
    // 2026-11-14: the frame went brighter and the stair's 10 -> 15 h change grew from +6.8 to +19: not this way.)
    if (voxFlags & 2)
        [unroll] for (int f = 0; f < 6; f++)
        {
            float3 s6 = 0;
            [unroll] for (int j4 = 0; j4 < 8; j4++)
                s6 += SixLoad(f, id * 2 + uint3(j4 & 1, (j4 >> 1) & 1, j4 >> 2)) * (mode == 1 ? a[j4] : 1);
            SixDstStore(f, id, s6 / 8);
        }
}

// The info panel (GI test keys), its rectangle in pixels in worldToPrevClip's first row (width 0: none): the frame
// darkened behind it, the text (coloured on black) added.
float3 Panel(uint2 id, float3 colour)
{
    float4 rect = worldToPrevClip[0];
    if (rect.z > 0 && all(id >= (uint2)rect.xy) && all(id < (uint2)(rect.xy + rect.zw)))
    {
        float3 text = depthTex.Load(int3(id - (uint2)rect.xy, 0)).rgb;
        float cover = max(text.r, max(text.g, text.b));
        colour = colour * 0.35 * (1 - cover) + text;
    }
    return colour;
}

// Debug view over the finished frame: the cells a camera ray meets first, as wire faces (mode 1, red: level 0) or solid
// in their colour (mode 2); top left the GI as lighting only (indirect light plus the share of the flat ambient kept).
// Mode 0: the info panel alone.
[numthreads(8, 8, 1)]
void view(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y)
        return;
    float2 uv = (id.xy + 0.5) / fullSize;
    float4 result = sceneTex.Load(int3(id.xy, 0));
    if (mode < 1)
    {
        viewOut[id.xy] = float4(Panel(id.xy, result.rgb), 1);
        return;
    }
    const float inset = 0.4;
    if (uv.x < inset && uv.y < inset)
    {
        float4 gi = colourTex.SampleLevel(linearClamp, uv / inset, 0);
        float share = saturate(gi.a - 2 * floor(gi.a * 0.5)); // a = share + 2 x the shader patch's flags
        viewOut[id.xy] = float4(Panel(id.xy, sqrt(Tone(gi.rgb + (1 - share) * 0.5))), 1);
        return;
    }
    float3 dirV = normalize(float3((uv.x * 2 - 1) * invProj.x, (1 - uv.y * 2) * invProj.y, -1));
    float3 dir = float3(dot(dirV, toWorldX.xyz), dot(dirV, toWorldY.xyz), dot(dirV, toWorldZ.xyz));
    float3 eye = float3(toWorldX.w, toWorldY.w, toWorldZ.w);
    int n = (int)gridMin.w;
    float travelled = 0; // metres from the eye: each level goes on where the finer one ended
    bool found = false;
    [loop] for (int level = 0; level < 4 && !found; level++)
    {
        float vs = voxelSize * (1 << level);
        int3 g = level == 0 ? (int3)gridMin.xyz : (int3)worldToPrevClip[level].xyz;
        float3 origin = (eye + dir * travelled) / vs; // in cells
        int3 cell = (int3)floor(origin);
        int3 stepDir = int3(dir.x >= 0 ? 1 : -1, dir.y >= 0 ? 1 : -1, dir.z >= 0 ? 1 : -1);
        float3 tDelta = 1 / max(abs(dir), 1e-6);
        float3 tMax = (dir >= 0 ? (cell + 1 - origin) : (origin - cell)) * tDelta;
        float t = 0;
        int axis = -1;
        [loop] for (int i = 0; i < 300; i++)
        {
            if (any(cell < g) || any(cell >= g + n))
                break;
            float4 v = level == 0 ? voxels[Wrap(cell)] : level == 1 ? voxels1[Wrap(cell)] : level == 2 ? voxels2[Wrap(cell)] : voxels3[Wrap(cell)];
            if (v.a > 0.5)
            {
                float3 hit = origin + dir * t - cell;
                float2 f = axis == 0 ? hit.yz : axis == 1 ? hit.xz : hit.xy;
                float2 e = axis < 0 ? 0.5 : min(f, 1 - f);
                float width = max((travelled + t * vs) * 2 * invProj.y / fullSize.y / vs, 0.01) * 1.5; // about 1.5 pixels, in cells
                bool edge = min(e.x, e.y) < width;
                float3 tint = level == 0 ? float3(1, 0, 0) : level == 1 ? float3(0, 1, 0) : level == 2 ? float3(0.2, 0.4, 1) : float3(1, 0.2, 1);
                if (mode >= 2)
                    result = float4(sqrt(Tone(v.rgb)) * (axis == 2 ? 1.0 : axis == 0 ? 0.85 : 0.7) * (edge ? 0.5 : 1), 1);
                else if (edge)
                    result = float4(tint, 1);
                found = true;
                break;
            }
            if (tMax.x < tMax.y && tMax.x < tMax.z)
            {
                cell.x += stepDir.x;
                t = tMax.x;
                tMax.x += tDelta.x;
                axis = 0;
            }
            else if (tMax.y < tMax.z)
            {
                cell.y += stepDir.y;
                t = tMax.y;
                tMax.y += tDelta.y;
                axis = 1;
            }
            else
            {
                cell.z += stepDir.z;
                t = tMax.z;
                tMax.z += tDelta.z;
                axis = 2;
            }
        }
        travelled += t * vs;
    }
    viewOut[id.xy] = float4(Panel(id.xy, result.rgb), 1);
}
)";

// GI geometry (Device::RunVoxelGeometry): the frame's z-prepass draws again, with their own vertex shaders and inputs,
// but the camera's view-projection in their constant buffer replaced by an orthographic projection of the level-0 grid
// along one axis (x, y, z in turn, one a frame): the viewport is the grid's N x N face, depth runs through its N cells.
// This pixel shader stamps every cell a fragment falls into (toroidal, as the grid) with the frame and an opacity:
// stamp * 256 + opacity * 255. Foliage is drawn without its alpha test (whole leaf cards), so its cells count as mostly
// open (CRYMT_GI_FOLIAGE, as Crytek's VegetationMaxOpacity). Root: the main signature plus u0 (table, parameter kVoxGeomUav) and b13 (constants,
// kVoxGeomConstants).
constexpr UINT kVoxGeomUav = 26, kVoxGeomConstants = 27;
constexpr int kVoxGeomLevels = 3; // grid levels that get geometry (0.25, 0.5, 1 m cells: 32, 64, 128 m)
const char* const kVoxGeomShader = R"(
cbuffer VoxGeom : register(b13)
{
    float4 grid;  // xyz the grid's first cell, w cells per axis
    float4 job;   // x the axis projected along, y the frame stamp, z the cell's opacity (1 solid, less for foliage)
};
RWTexture3D<uint> occupancy : register(u0);

void voxelize(float4 pos : SV_Position)
{
    int n = (int)grid.w;
    int3 uvw = int3((int)pos.x, (int)pos.y, (int)(pos.z * n));
    // The first and last slice along the projection are left out: vertex shaders that pin depth to a plane (far
    // geometry and sky at the far plane, shadow pancaking at the near) put whole faces there, a wall of cells at the
    // grid's edge (the user: a cube like a sky dome around the camera in the Num2 view, walls at the voxels' reach).
    if (any(uvw < 0) || any(uvw >= n) || uvw.z == 0 || uvw.z == n - 1)
        return;
    int axis = (int)job.x;
    int3 local = axis == 0 ? int3(uvw.z, uvw.x, uvw.y) : axis == 1 ? int3(uvw.x, uvw.z, uvw.y) : uvw;
    int3 cell = (int3)grid.xyz + local;
    uint old;
    InterlockedMax(occupancy[(cell % n + n) % n], (uint)job.y * 256 + (uint)(saturate(job.z) * 255), old); // the most opaque of a frame
}
)";

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
    bool plainCopy = false;     // a pixel shader that only samples one texture into its output (IsPlainCopy)

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
                    writesTarget2 = true;
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
    // 3 (the default since 2026-11-13 night): per face. Village, each face drawn with every mode and compared with no
    // culling (closed meshes look the same with the right one): faces 0 1 2 4 5 the inverted one (11-15% of pixels off,
    // the noise of the faces' blending; the engine's 50-88%), face 3 the engine's (5% against 27%: its image is not
    // mirrored; the walls had holes in it - the user).
    static int EngineCubeCullFor(int face)
    {
        static constexpr int kPerFace[6] = {2, 2, 2, 0, 2, 2};
        const int mode = EngineCubeCull();
        return mode < 3 ? mode : face >= 0 && face < 6 ? kPerFace[face] : 2;
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
    // A material's own environment map replaced: the voxel cube / 2D map (mode 1), the engine's cube (mode 2: cube maps only).
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
        float radius = 2.5f, bounce = 1.0f, occlusion = 0.8f, temporal = 0.75f, maxDistance = 120.0f, debug = 0.0f;
        bool upsample = true;    // CRYMT_GI_UPSAMPLE=0: the engine reads the half-size GI directly (v1.3)
        bool fixedNoise = false; // CRYMT_GI_FIXEDNOISE=1: the same sample pattern every frame
        bool nearest = false;    // CRYMT_GI_NEAREST=1: a GI texel stands for the nearest depth of its 2x2 block
        bool plainBounce = false; // CRYMT_GI_FIREFLY=0: the screen part's bounce as a plain sum (no bright-sample damping)
        bool narrowNormals = false; // CRYMT_GI_NARROWNORMALS=1: normals one GI texel wide only (faces show on round meshes)
        bool weaponGi = true;       // CRYMT_GI_WEAPON=0: no GI on the first-person view (weapon, hands, held objects)
        int directions = 2;         // CRYMT_GI_DIRECTIONS: screen-part directions a GI texel, 2 (turned each frame) or 4
        float occFar = 16;          // CRYMT_GI_OCCRANGE (m): the cones' occlusion full to half of it, gone at it (0: no limit)
        float occStrength = 1;      // CRYMT_GI_OCCSTRENGTH: occlusion strength under cover (Num4 / Num7 multiply it)
        bool voxels = false;     // CRYMT_GI_VOXELS=1: the voxel grid around the camera (kVoxShader)
        float voxelSize = 0.25f; // CRYMT_GI_VOXELSIZE, metres (the finest level)
        float voxelView = 0;     // CRYMT_GI_VOXELVIEW: 1 wire faces over the frame + GI inset, 2 solid voxels (implies VOXELS)
        float voxelMix = 0;      // CRYMT_GI_VOXELS value: 1 cones through the grid in the GI, 2 their part only, 3 the screen part only, 4 the grid without cones
        float voxelStrength = 1.5f, voxelReach = 64.0f; // CRYMT_GI_VOXELSTRENGTH (bounce light of the cones), CRYMT_GI_VOXELREACH (m)
        float voxelOcclusion = 0.6f; // CRYMT_GI_VOXELAO: how much of the flat ambient the cones' occlusion removes
        bool geometry = true;        // the z-prepass draws voxelized into level 0 (RunVoxelGeometry); CRYMT_GI_GEOMETRY=0 off
                                     // (default on since 2026-11-06, the user's choice: ~5-7 FPS)
        bool geometryFoliage = false; // CRYMT_GI_GEOMETRY=2: foliage too (mostly open cells)
        bool geometryPersist = true;  // CRYMT_GI_GEOMPERSIST=0: voxelized geometry only from the last three voxelizations
        bool geometryCache = false;   // CRYMT_GI_GEOMCACHE=1 (off by default since 2026-11-11: the user saw the shading flicker
                                      // with it - a cell shared with foliage or a moving object takes the newest stamp, so the
                                      // cached object won only one frame in four); on: an object
                                      // drawn unchanged (same buffers, same place in the world) every 4th frame, moved, new or
                                      // animated ones every time (with kept geometry only)
        bool opacityGeometry = true;  // the cells' opacity from the voxelized geometry only, the screen gives only their colour
                                      // (levels with geometry; the coarsest keeps the screen); CRYMT_GI_OCCUPANCY=screen: also
                                      // from the screen (before 2026-11-10: 12 x the cells coming and going on a still camera)
        bool screenPart = false;      // CRYMT_GI_SCREEN=1: the screen part (bounce and occlusion traced on screen) with the
                                      // cones; off by default since 2026-11-10 (the user), it is not traced then
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
        float outdoorStrength = 1.0f;      // CRYMT_GI_OUTDOOR: the occlusion strength outdoors (Num4 / Num7 set the indoor one)
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
        bool probePrecise = false;         // CRYMT_GI_PROBEPRECISE=1: the probes' rays as before the speed-ups (2026-11-14,
                                           // 3.2 ms for two cascades against 1.17; the panel's "Probe rays", to compare)
        float probeStrength = 1.0f;        // CRYMT_GI_PROBESTRENGTH: their bounce light's factor (the cones' is voxelStrength
                                           // 1.5: coarse mips lose light; the probes' thin rays see the cells' full light,
                                           // and at 1.5 the loop through the screen's colour lit closed rooms by itself)
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
        float terrainAoNear = 30.0f;       // CRYMT_GI_TERRAINAONEAR: metres up to which the GI alone works; over 1.5 x that
                                           // the GI fades out and the baked terms in (far trees are sprites, beyond the voxels)
        bool gtaoFull = false;             // CRYMT_GI_GTAOFULL=1: GTAO at full size (island 5120x1440: 3.5 ms against 1.2)
        bool gtaoNoFoliage = false;        // CRYMT_GI_GTAOFOLIAGE=0: grass and leaves (the foliage mask) occlude nothing in GTAO
                                           // (the user: plants looked worse without their occlusion)
        float grassGi = 1.0f;              // CRYMT_GI_GRASSGI: the same for grass (marked apart by the z-prepass), which keeps
                                           // the engine's SSAO in place of GTAO (2026-11-10, the user: grass flat, too bright)
        float foliageGi = 1.5f;            // CRYMT_GI_FOLIAGEGI: the GI's occlusion and bounce on grass and leaves (the foliage
                                           // mask) times this (the user: more of the voxel GI on grass); 1 as everything else
        bool particlesGi = true;           // CRYMT_GI_PARTICLES=0 (key "-"): blended particles (smoke, dust) without the GI
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
            read(L"CRYMT_GI_RADIUS", v.radius);
            read(L"CRYMT_GI_BOUNCE", v.bounce);
            read(L"CRYMT_GI_AO", v.occlusion);
            read(L"CRYMT_GI_TEMPORAL", v.temporal);
            read(L"CRYMT_GI_DISTANCE", v.maxDistance);
            read(L"CRYMT_GI_DEBUG", v.debug);
            if (v.debug == 0 && GetEnvironmentVariableW(L"CRYMT_GI_SCREENCLASSIC", text, 32) && wcscmp(text, L"0") != 0)
                v.debug = 5; // the screen part's horizon method (before the visibility bitmask)
            v.upsample = !(GetEnvironmentVariableW(L"CRYMT_GI_UPSAMPLE", text, 32) && wcscmp(text, L"0") == 0);
            v.fixedNoise = GetEnvironmentVariableW(L"CRYMT_GI_FIXEDNOISE", text, 32) && wcscmp(text, L"0") != 0;
            v.nearest = GetEnvironmentVariableW(L"CRYMT_GI_NEAREST", text, 32) && wcscmp(text, L"0") != 0;
            v.plainBounce = GetEnvironmentVariableW(L"CRYMT_GI_FIREFLY", text, 32) && wcscmp(text, L"0") == 0;
            v.narrowNormals = GetEnvironmentVariableW(L"CRYMT_GI_NARROWNORMALS", text, 32) && wcscmp(text, L"0") != 0;
            v.weaponGi = !(GetEnvironmentVariableW(L"CRYMT_GI_WEAPON", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_GI_OCCRANGE", v.occFar);
            read(L"CRYMT_GI_OCCSTRENGTH", v.occStrength);
            if (GetEnvironmentVariableW(L"CRYMT_GI_DIRECTIONS", text, 32))
                v.directions = _wtoi(text) <= 2 ? 2 : 4;
            // The voxel cones come with GI (the screen part alone looks poor); CRYMT_GI_VOXELS=0 turns them off.
            const bool voxelVar = GetEnvironmentVariableW(L"CRYMT_GI_VOXELS", text, 32) != 0;
            v.voxels = voxelVar ? wcscmp(text, L"0") != 0 : v.on;
            v.voxelMix = !v.voxels ? 0.0f : voxelVar ? static_cast<float>(_wtof(text)) : 1.0f;
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
            v.geometryCache = GetEnvironmentVariableW(L"CRYMT_GI_GEOMCACHE", text, 32) && wcscmp(text, L"0") != 0;
            read(L"CRYMT_GI_FOLIAGE", v.foliageOpacity);
            v.opacityGeometry = !(GetEnvironmentVariableW(L"CRYMT_GI_OCCUPANCY", text, 32) && _wcsicmp(text, L"screen") == 0);
            v.screenPart = GetEnvironmentVariableW(L"CRYMT_GI_SCREEN", text, 32) && wcscmp(text, L"0") != 0;
            if (v.voxelMix == 1 && !v.screenPart)
                v.voxelMix = 2; // the cones' part only
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
            v.probePrecise = GetEnvironmentVariableW(L"CRYMT_GI_PROBEPRECISE", text, 32) && wcscmp(text, L"0") != 0;
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
            v.gtaoNoFoliage = GetEnvironmentVariableW(L"CRYMT_GI_GTAOFOLIAGE", text, 32) && wcscmp(text, L"0") == 0;
            read(L"CRYMT_GI_FOLIAGEGI", v.foliageGi);
            read(L"CRYMT_GI_GRASSGI", v.grassGi);
            v.particlesGi = !(GetEnvironmentVariableW(L"CRYMT_GI_PARTICLES", text, 32) && wcscmp(text, L"0") == 0);
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
        g.bounce = b.bounce * bounce;
        g.voxelStrength = b.voxelStrength * bounce;
        g.probeStrength = b.probeStrength * bounce; // GI v2 too (the user liked Sponza's strong bounce)
        g.voxels = b.on; // the grid keeps up, so the cones and the view switch on at once
        if (g_giControl.parts < 0)
            g_giControl.parts = b.voxelMix == 2 ? 1 : 0;
        const int parts = g_giControl.parts; // Num1: both parts, the cones only
        g.voxelMix = parts == 1 ? 2.0f : (b.voxelMix > 0 && b.voxelMix < 4 && b.voxelMix != 2 ? b.voxelMix : 1.0f);
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
        if (g_giControl.geomCache < 0)
            g_giControl.geomCache = b.geometryCache ? 1 : 0;
        g.geometryCache = g_giControl.geomCache == 1;
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
        g.probePrecise = g_giControl.probePrecise >= 0 ? g_giControl.probePrecise > 0 : b.probePrecise;
        if (g_giControl.particles < 0)
            g_giControl.particles = b.particlesGi ? 1 : 0;
        g.particlesGi = g_giControl.particles > 0;
        if (g_giControl.particleOut < 0)
            g_giControl.particleOut = static_cast<int>(b.particleOutdoor * 100.0f + 0.5f);
        g.particleOutdoor = g_giControl.particleOut / 100.0f;
        if (g_giControl.bakedOff < 0)
            g_giControl.bakedOff = b.bakedOff;
        g.bakedOff = g_giControl.bakedOff & 30;
    }

    void GiSceneDone(const DlssSceneInfo* info)
    {
        if (!Gi().on || !info || !info->valid || !m_rtv[0] || !m_dsv)
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

    void UpdateFrameTexture(const DlssSceneInfo* info)
    {
        if (!m_jitterTex)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = 8;
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
        float data[32] = {};
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
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 1, 1, 256};
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
        g_wantPassMatrices.store(Gi().on && Gi().geometry && (Gi().shadowGeometry || Gi().cubeGeometry || Gi().cubeInject),
                                 std::memory_order_relaxed);
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
        D3D12_DESCRIPTOR_RANGE ranges[3] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 3},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kGiVoxTable, 3, 0, 0}};
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = kGiConstants;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // the voxel grid levels (t3..t6), per axis (t7..t10)
        params[2].DescriptorTable = {1, ranges + 2};
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
        // The foliage mask: whether grass and leaves cast (CRYMT_CONTACTFOLIAGE=0: they do not).
        Srv12* foliage = !s.contactFoliage && m_foliageSrv && m_foliageFrame == m_frame ? m_foliageSrv : nullptr;
        Require(z, kSrvState);
        if (foliage)
            Require(foliage->info, kSrvState);
        if (hw)
            Require(hw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(&m_sss, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
        g.dev->CreateUnorderedAccessView(m_sss.res, nullptr, &ud, {cpu.ptr + 3 * g.srvInc});

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
            f[(kVisFloat4s - 1) * 4] = Gi().probePrecise ? 1.0f : 0.0f; // [161].x: the probes' precise rays
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        // Three tables of 6: trace (depth, colour, -, raw out, -, -), filter (depth, raw, previous GI, new GI out,
        // distance out, previous distance) and upsample (depth, new GI, -, full-size out, distances, -).
        const UINT aoTables = 24 + kGiVoxTable; // then three tables of 6 for the GTAO passes, the particles' volume, the
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
            g.dev->CreateShaderResourceView(probeCur, &pd, at(24 + kGiVoxTable - 2));
            g.dev->CreateShaderResourceView(probeDepthCur, &pdd, at(24 + kGiVoxTable - 1));
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
                                 s.radius, s.bounce, s.occlusion, m_giReset ? 0.0f : s.temporal,
                                 s.maxDistance, static_cast<float>(m_giFrame & 1023), s.debug, (s.fixedNoise ? 1.0f : 0.0f) + (s.nearest ? 2.0f : 0.0f) + (s.plainBounce ? 4.0f : 0.0f) + (s.narrowNormals ? 8.0f : 0.0f) +
                                 (cones && s.weaponGi && m_giCam && m_giPsoCamera ? 16.0f : 0.0f) + (s.directions <= 2 ? 32.0f : 0.0f) +
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
            D3D12_RESOURCE_BARRIER toRead[2] = {Transition(probeCur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                                Transition(probeDepthCur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            m_list->ResourceBarrier(2, toRead);
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
                const float foliageGi = std::min(s.foliageGi, 3.99f) + 4.0f * std::floor(std::max(s.grassGi, 0.0f) * 100.0f + 0.5f);
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
        if (cones && s.particlesGi && m_pvol && m_pvolPso)
        {
            // The particles' occlusion volume around the camera, from the grid (its parameters in reproj's rows).
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
    bool ShadowRecording()
    {
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
        // Unchanged objects (kept geometry only: their stamps stay): each level takes them in turns, every 4th frame
        // (level 0 every 4th of its updates, level 1 every 2nd, level 2 each one), by the key's phase so the work spreads
        // evenly; 4 against 3 axes, so each turn comes along the next axis. New ones (first seen, or seen again after a
        // gap) the first three frames: all three axes at once. Moved or animated ones every time, as all were before.
        const bool cache = !cubeOnly && Gi().geometryCache && Gi().geometryPersist && m_voxValid;
        std::vector<uint8_t> due(m_voxDraws.size(), 1);
        if (cache)
        {
            const uint64_t now = m_giFrame + 1;
            for (size_t i = 0; i < m_voxDraws.size(); i++)
            {
                const VoxDraw& d = m_voxDraws[i];
                if (d.animated)
                {
                    m_stats.voxGeomAnimated++;
                    continue;
                }
                VoxSeen& s = m_voxKeys[d.key];
                if (s.last + 2 < now)
                    s.first = now; // new, or back after a gap
                s.last = now;
                if (now - s.first < 3)
                {
                    m_stats.voxGeomFresh++;
                    continue;
                }
                due[i] = 0; // in turns, per level below
            }
            if ((m_giFrame & 255) == 0)
                for (auto it = m_voxKeys.begin(); it != m_voxKeys.end();)
                    it = it->second.last + 64 < now ? m_voxKeys.erase(it) : std::next(it);
        }
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
            const uint64_t turn = job.shadow || job.cube ? 0 : m_giFrame / kVoxPeriod[l]; // this level's update count
            const uint64_t turns = job.shadow || job.cube ? 1 : std::max(1, 4 / kVoxPeriod[l]);
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
                if (!job.shadow && !job.cube && !due[di] && (turn + (d.key >> 32)) % turns != 0)
                {
                    m_stats.voxGeomInTurn++;
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
                // With the cache unchanged objects come every 4th frame: the window at least two of their turns.
                const uint32_t window = static_cast<uint32_t>(std::max(3 * kVoxPeriod[l], Gi().geometryCache ? 8 : 0) - 1);
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
        static const char* const parts[] = {"ЭКРАН+ВОКСЕЛИ|SCREEN+VOXELS", "ВОКСЕЛИ|VOXELS"};
        static const char* const ao[] = {"SSAO ИГРЫ|GAME'S SSAO", "GTAO"};
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
        static const char* const cache[] = {"КАЖДЫЙ КАДР|EVERY FRAME", "КАЖДЫЙ 4-Й|EVERY 4TH"};
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
        static const char* const probeRays[] = {"БЫСТРЫЕ|FAST", "ТОЧНЫЕ (ДОРОЖЕ)|PRECISE (SLOWER)"};
        static const char* const cubePicture[] = {"Num4/6: СНЯТЬ|Num4/6: TAKE", "Num4/6: СНЯТЬ|Num4/6: TAKE"};
        static const char* const cubeRate[] = {"КАЖДЫЙ КАДР|EVERY FRAME", "РАЗ В 2 КАДРА|EVERY 2ND FRAME", "РАЗ В 3 КАДРА|EVERY 3RD FRAME",
                                               "РАЗ В 4 КАДРА|EVERY 4TH FRAME", "РАЗ В 6 КАДРОВ|EVERY 6TH FRAME", "РАЗ В 8 КАДРОВ|EVERY 8TH FRAME"};
        static const int outdoorValues[5] = {50, 70, 100, 150, 200};
        static const char* const dlssModel[] = {"K (ТРАНСФОРМЕР)|K (TRANSFORMER)", "J", "L (DLSS 4.5)|L (DLSS 4.5)", "M (DLSS 4.5)|M (DLSS 4.5)",
                                                "F (CNN)|F (CNN)", "E (CNN)|E (CNN)"};
        static const char* const dlssCompare[] = {"ВЫКЛ|OFF", "СПРАВА K|RIGHT: K", "СПРАВА J|RIGHT: J", "СПРАВА L|RIGHT: L", "СПРАВА M|RIGHT: M",
                                                  "СПРАВА F|RIGHT: F", "СПРАВА E|RIGHT: E"};
        static const std::vector<PanelItem> items = {
            // GI
            {0, "GI|GI", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.enabled ? 1 : 0; }, [] { return 1; },
             [](int v) { g_giControl.enabled = v != 0; }, true, nullptr},
            {0, "Части GI|GI parts", 0, 1, 1, true, parts, 0, nullptr,
             [] { const int v = g_giControl.parts; return v < 0 ? (GiBase().voxelMix == 2 ? 1 : 0) : v; },
             [] { return GiBase().voxelMix == 2 ? 1 : 0; }, [](int v) { g_giControl.parts = v; }, true, nullptr},
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
            // The probes' rays (CRYMT_GI_PROBEPRECISE): as before the speed-ups of 2026-11-14 (3.2 ms) to compare.
            {0, "Лучи проб|Probe rays", 0, 1, 1, true, probeRays, 0, nullptr,
             [] { const int v = g_giControl.probePrecise; return v < 0 ? (GiBase().probePrecise ? 1 : 0) : std::min(v, 1); },
             [] { return GiBase().probePrecise ? 1 : 0; }, [](int v) { g_giControl.probePrecise = v; }, true, nullptr},
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
            {2, "Неизменные объекты|Unchanged objects", 0, 1, 1, true, cache, 0, nullptr,
             [] { const int v = g_giControl.geomCache; return v < 0 ? (GiBase().geometryCache ? 1 : 0) : v; },
             [] { return GiBase().geometryCache ? 1 : 0; }, [](int v) { g_giControl.geomCache = v; }, true, nullptr},
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
            {3, "Сущности и облака в кубкарте|Entities and clouds in the cube", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.envCubeEntities; return v < 0 ? EngineCubeEntitiesBase() : v; }, [] { return EngineCubeEntitiesBase(); },
             [](int v) { g_giControl.envCubeEntities = v; }, true, nullptr},
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
    float m_frameLast[32] = {1e9f};             // the frame texture's last data
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
    ID3D12PipelineState* m_giPso = nullptr;       // trace
    ID3D12PipelineState* m_giPsoFilter = nullptr; // blur + temporal blend
    ID3D12Resource* m_giRaw = nullptr;            // traced result before the filter (unordered access between passes)
    ID3D12PipelineState* m_giPsoUpsample = nullptr; // full-size GI by distance
    ID3D12PipelineState* m_giPsoCamera = nullptr;   // the first-person view's GI value
    ID3D12Resource* m_giCam = nullptr;              // its 1x1 texture
    ID3D12PipelineState* m_giPsoProbes = nullptr;   // GI v2: the probes' rays and atlas (kGiShader "probes")
    ID3D12Resource* m_probeAtlas[2] = {};           // their atlases, written in turn (1024 x kProbeCascades by 1152 RGBA16F: tiles, states)
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
    struct VoxSeen
    {
        uint64_t first = 0, last = 0; // frames + 1: since when seen without a gap, last seen
    };
    std::unordered_map<uint64_t, VoxSeen> m_voxKeys; // GI geometry draws by key (RunVoxelGeometry's turns)
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
    if (Gi().on && m_giHist[0] && m_giLastRun != m_giFrame && m_ps && PsUsesGiPlaceholder())
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
    if (m_ps && m_ps->writesTarget1 && !m_ps->writesTarget2 && !(m_vs && m_vs->motionBlur) && m_rtv[0] && !m_rtv[1] && m_rtv[0]->format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        m_rtv[0]->samples == 1 && m_foliageW && static_cast<const TextureCore*>(m_rtv[0]->info)->width == m_foliageW &&
        static_cast<const TextureCore*>(m_rtv[0]->info)->height == m_foliageH)
        BindBiasMask();
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

// Textures/CryMT/jitter.dds: CryMT's 8x1 placeholder for the frame texture (patched ShadowMaskGen.cfx): texels 0..6
// zero (what the shader reads: no jitter, no contact shadows without CryMT), the last 5A5A5A5A (the mark).
bool IsJitterPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 8 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 32; i++)
        if (p[i] != (i < 28 ? 0 : 0x5A))
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
        static uint64_t envBindsBefore;
        Log("environment cube binds per frame: %.1f", (g_device->m_envBinds - envBindsBefore) / 3600.0);
        envBindsBefore = g_device->m_envBinds;
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
