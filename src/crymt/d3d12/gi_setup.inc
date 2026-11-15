// CryMT D3D12 back end, class Device: the GI's textures, scene depth and pipelines, GI v2 probes.
// Included inside the class in D3D12Backend.cpp, where this text stood.

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
        dd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // xy the distances' moments, z the short-term luminance (probes)
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
