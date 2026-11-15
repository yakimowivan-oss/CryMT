// CryMT D3D12 back end: Device: init, command lists, submission, pipelines, applying the state.
// Included in D3D12Backend.cpp, where this text stood.
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

