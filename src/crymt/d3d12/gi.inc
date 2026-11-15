// CryMT D3D12 back end, class Device: the GI pass (RunGi).
// Included inside the class in D3D12Backend.cpp, where this text stood.

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
            f[(kVisFloat4s - 1) * 4 + 0] = Gi().probeFast ? 1.0f : 0.0f;   // [161].x: the probes' faster response (probes)
            // [161].y..z: free (canopy, foliage smoothing removed 2026-11-15)
            f[(kVisFloat4s - 1) * 4 + 3] = Gi().gtaoHistoryWeight;          // [161].w: GTAO's history weight (gtaoBlur)
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        // Three tables of 6: trace (depth, colour, -, raw out, -, -), filter (depth, raw, previous GI, new GI out,
        // distance out, previous distance) and upsample (depth, new GI, -, full-size out, distances, -).
        const UINT aoTables = 24 + kGiVoxTable + 1; // (+1: u3 probeMatch) then three tables of 6 for the GTAO passes, the particles' volume, the
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
            pdd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g.dev->CreateShaderResourceView(probeCur, &pd, at(24 + kGiVoxTable - 3));
            g.dev->CreateShaderResourceView(probeDepthCur, &pdd, at(24 + kGiVoxTable - 2));
            // t43: the foliage mask for the upsample's smoothing on grass and leaves (null: none)
            const bool foliageOk = false && m_foliageSrv && m_foliageFrame == m_frame && m_foliageW == m_giFullW && m_foliageH == m_giFullH;
            if (foliageOk)
            {
                m_foliageSrv->Refresh();
                Require(m_foliageSrv->info, kSrvState);
            }
            g.dev->CopyDescriptorsSimple(1, at(24 + kGiVoxTable - 1), foliageOk ? m_foliageSrv->cpu : g_nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            // u3 after the second table's textures: probeMatch (a null view with probes off)
            D3D12_UNORDERED_ACCESS_VIEW_DESC md{};
            md.Format = DXGI_FORMAT_R32_UINT;
            md.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            md.Buffer.NumElements = 16;
            g.dev->CreateUnorderedAccessView(probesOn ? m_probeMatch : nullptr, nullptr, &md, at(24 + kGiVoxTable));
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
                                 0.0f, 0.0f, 0.0f, m_giReset ? 0.0f : s.temporal,
                                 s.maxDistance, static_cast<float>(m_giFrame & 1023), s.debug, (s.fixedNoise ? 1.0f : 0.0f) + (s.nearest ? 2.0f : 0.0f) + (s.narrowNormals ? 8.0f : 0.0f) +
                                 (cones && s.weaponGi && m_giCam && m_giPsoCamera ? 16.0f : 0.0f) +
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
            D3D12_RESOURCE_BARRIER toRead[3] = {Transition(probeCur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                                Transition(probeDepthCur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                                {}};
            toRead[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // probeMatch: the ratio set and the sums cleared before the trace
            toRead[2].UAV.pResource = m_probeMatch;
            m_list->ResourceBarrier(3, toRead);
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
            FlushBarriers(); // the foliage mask (t43) readable
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
                const float foliageGi = std::min(s.foliageGi, 3.99f) + 4.0f * std::floor(std::min(std::max(s.grassGi, 0.0f), 10.0f) * 100.0f + 0.5f) +
                                        4096.0f * std::floor(std::min(std::max(s.foliageBounce, 0.0f), 3.0f) * 100.0f + 0.5f);
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
        if (cones && (s.particlesGi || s.vfog) && m_pvol && m_pvolPso)
        {
            // The particles' occlusion volume around the camera, from the grid (its parameters in reproj's rows); also
            // for the GI in the fog (RunVfog), with the particles' own GI off.
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
            m_pvolFrame = m_frame;
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
