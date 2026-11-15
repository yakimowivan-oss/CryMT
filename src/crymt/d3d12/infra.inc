// CryMT D3D12 back end: logging, fences, deferred work, allocators, uploads, heaps, copy records, private data.
// Included in D3D12Backend.cpp, where this text stood.
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

