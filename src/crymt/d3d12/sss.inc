// CryMT D3D12 back end, class Device: Bend's screen-space shadows.
// Included inside the class in D3D12Backend.cpp, where this text stood.

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
