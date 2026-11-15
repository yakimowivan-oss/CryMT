// CryMT D3D12 back end, class Device: the engine cube's copies, the materials' cube and 2D maps.
// Included inside the class in D3D12Backend.cpp, where this text stood.

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
