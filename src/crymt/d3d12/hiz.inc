// CryMT D3D12 back end, class Device: the water SSR's depth pyramid.
// Included inside the class in D3D12Backend.cpp, where this text stood.

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
