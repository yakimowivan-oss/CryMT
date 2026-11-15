// CryMT D3D12 back end, class Device: the GI in the fog (froxel volume).
// Included inside the class in D3D12Backend.cpp, where this text stood.

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
