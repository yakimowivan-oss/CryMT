// CryMT D3D12 back end, class Device: DLSS: targets, exposure, sanitizing, motion vectors.
// Included inside the class in D3D12Backend.cpp, where this text stood.

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
