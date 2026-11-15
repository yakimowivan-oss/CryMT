// CryMT D3D12 back end: uploads, transitions, pipeline keys and states, statistics, the pipeline cache and its workers.
// Included in D3D12Backend.cpp, where this text stood.
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
