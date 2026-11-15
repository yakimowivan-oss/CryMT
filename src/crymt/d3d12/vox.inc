// CryMT D3D12 back end, class Device: the voxel grids: textures, recording, geometry, updates.
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // Voxel grid (kVoxShader): pipeline and the grid texture on first use.
    bool EnsureVox()
    {
        if (m_voxFailed)
            return false;
        if (m_vox[0])
            return true;
        m_voxFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile || !g_serializeRoot)
            return false;
        ID3DBlob* code[6] = {};
        ID3DBlob* errors = nullptr;
        const char* entries[6] = {"update", "view", "downsample", "light", "roof", "inject"};
        for (int i = 0; i < 6; i++)
            if (FAILED(compile(kVoxShader, strlen(kVoxShader), "CryMT_Vox", nullptr, nullptr, entries[i], "cs_5_0", 1 << 15, 0, &code[i], &errors)))
            {
                Log("voxel shader %s: %s", entries[i], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
                return false;
            }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 20, 0, 0, 3}};
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = kVoxConstants;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b1: the lamps (light)
        params[2].Descriptor.ShaderRegister = 1;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b2: voxFlags (VoxFlags())
        params[3].Constants.ShaderRegister = 2;
        params[3].Constants.Num32BitValues = 1;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (int i = 0; i < 2; i++)
        {
            samplers[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
        }
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 4;
        rd.pParameters = params;
        rd.NumStaticSamplers = 2;
        rd.pStaticSamplers = samplers;
        ID3DBlob* blob = nullptr;
        if (FAILED(g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&m_voxRoot))))
        {
            Log("voxel root signature failed");
            return false;
        }
        blob->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_voxRoot;
        HRESULT hr = S_OK;
        ID3D12PipelineState** psos[6] = {&m_voxPsoUpdate, &m_voxPsoView, &m_voxPsoDown, &m_voxPsoLight, &m_voxPsoRoof, &m_voxPsoInject};
        for (int i = 0; i < 6; i++)
        {
            pd.CS = {code[i]->GetBufferPointer(), code[i]->GetBufferSize()};
            if (SUCCEEDED(hr))
                hr = g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(psos[i]));
            code[i]->Release();
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        d.Width = d.Height = kVoxN;
        d.DepthOrArraySize = kVoxN;
        d.MipLevels = kVoxMips;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_vox[l]));
        // The cells' direction of light (CRYMT_GI_DIRLIGHT 1, VoxFlags): RGBA8 SNORM, xyz g with |g| <= 1, the light going
        // out towards w is the colour times 1 + g.w; the same mips as the grid. ~37 MB for four levels.
        d.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxL1[l]));
        d.Width = d.Height = kVoxN / 2;
        d.DepthOrArraySize = kVoxN / 2;
        d.MipLevels = kVoxMips - 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxDir[l]));
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(hr))
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxRoof));
        Log("voxel grid %d levels of %d^3 (%.2f m cells and up): %s", kVoxLevels, kVoxN, Gi().voxelSize, SUCCEEDED(hr) ? "ready" : "failed");
        for (auto& level : m_voxPrevGrid)
            for (int& v : level)
                v = -(1 << 28); // the first update resets every cell
        m_voxFailed = FAILED(hr);
        return SUCCEEDED(hr);
    }

    // kVoxTable descriptors (t0..t2, u0..u4) per table, count tables, on the shader-visible ring.
    void VoxTable(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu, UINT count = 1)
    {
        while (!m_srvRing.TryAlloc(kVoxTable * count, cpu, gpu))
        {
            Submit();
            WaitFence(m_srvRing.OldestFence() ? m_srvRing.OldestFence() : g.signaled);
            PollFence();
            m_srvRing.Reclaim();
        }
    }

    // A UAV of one mip of a grid level, or a null 3D view.
    void VoxUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 ? nullptr : m_vox[level], nullptr, &ud, at);
    }

    // A UAV of one mip of a level's light direction (m_voxL1), or a null 3D view.
    void L1Uav(D3D12_CPU_DESCRIPTOR_HANDLE at, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 || level < 0 ? nullptr : m_voxL1[level], nullptr, &ud, at);
    }

    // The voxel passes' b2 (voxFlags): bit 0 the cells' light direction (L1) is kept (CRYMT_GI_DIRLIGHT 1 and 2), bit 1
    // the six directions too (2, once made).
    uint32_t VoxFlags() const { return (Gi().dirLight >= 1 ? 1u : 0u) | (Gi().dirLight == 2 && m_voxSixReady ? 2u : 0u); }
    // What the GI's cones use (visAreas[0].z): 0 one colour, 1 L1, 2 six directions.
    int ConeDirMode() const { return Gi().dirLight == 2 && m_voxSixReady ? 2 : Gi().dirLight >= 1 ? 1 : 0; }

    // The six directions (CRYMT_GI_DIRLIGHT 2): per level the light going out towards +X, -X, +Y, -Y, +Z, -Z, R11G11B10
    // with the grid's mips, made when first asked for (~220 MB for four levels). Their contents start undefined: the
    // grid is reset once (every cell "entered", which clears them).
    bool EnsureVoxSix()
    {
        if (m_voxSixReady || m_voxSixFailed)
            return m_voxSixReady;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        d.Width = d.Height = kVoxN;
        d.DepthOrArraySize = kVoxN;
        d.MipLevels = kVoxMips;
        d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = S_OK;
        for (int l = 0; l < kVoxLevels && SUCCEEDED(hr); l++)
            for (int f = 0; f < 6 && SUCCEEDED(hr); f++)
                hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                    __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_voxSix[l][f]));
        m_voxSixReady = SUCCEEDED(hr);
        m_voxSixFailed = !m_voxSixReady;
        if (m_voxSixReady)
            for (auto& level : m_voxPrevGrid)
                for (int& v : level)
                    v = -(1 << 28); // the next update resets every cell (and its six directions)
        Log("voxel six directions (CRYMT_GI_DIRLIGHT 2): %s", m_voxSixReady ? "ready" : "failed");
        return m_voxSixReady;
    }

    // A UAV of one mip of a level's direction face f (m_voxSix), or a null 3D view.
    void SixUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int f, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 || level < 0 || !m_voxSixReady ? nullptr : m_voxSix[level][f], nullptr, &ud, at);
    }

    // A UAV of one mip of a level's per-axis opacity (grid mip mip + 1), or a null 3D view.
    void DirUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int mip, int level = 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip < 0 ? 0 : static_cast<UINT>(mip);
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(mip < 0 ? nullptr : m_voxDir[level], nullptr, &ud, at);
    }

    void NullSrv(D3D12_CPU_DESCRIPTOR_HANDLE at)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(nullptr, &nd, at);
    }

    // The constants both voxel passes share: the camera (worldToView, toWorld), projection, grid.
    void VoxConstants(float* c, const float* toWorld, UINT width, UINT height, float mode, int level = 0)
    {
        memcpy(c, m_giView, 16 * sizeof(float));
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 4; i++)
                c[32 + j * 4 + i] = toWorld[i * 4 + j];
        const float rest[16] = {static_cast<float>(width), static_cast<float>(height), 1.0f / m_giProj[0], 1.0f / m_giProj[5],
                                m_giProj[10], m_giProj[14], Gi().voxelSize * static_cast<float>(1 << level), mode,
                                static_cast<float>(m_voxGrid[level][0]), static_cast<float>(m_voxGrid[level][1]), static_cast<float>(m_voxGrid[level][2]),
                                static_cast<float>(kVoxN), static_cast<float>(m_voxPrevGrid[level][0]), static_cast<float>(m_voxPrevGrid[level][1]),
                                static_cast<float>(m_voxPrevGrid[level][2]), 0.0f};
        memcpy(c + 44, rest, sizeof(rest));
    }

    // Grid levels are updated every frame (0), every 2nd (1), 4th (2), 8th (3); all of them while the grid is new. The
    // geometry pass voxelizes a level only in the frames it is updated.
    static constexpr int kVoxPeriod[kVoxLevels] = {1, 2, 4, 8};
    bool VoxUpdates(int level) const
    {
        static constexpr uint64_t kPhase[kVoxLevels] = {0, 0, 1, 3};
        return !m_voxValid || (m_giFrame % kVoxPeriod[level]) == kPhase[level];
    }

    // ---- GI geometry: the z-prepass draws voxelized into level 0 (kVoxGeomShader)
    struct VoxDraw
    {
        const std::vector<uint8_t>* vsCode;
        uint64_t vsHash;
        Layout12* layout;
        D3D10_PRIMITIVE_TOPOLOGY topology;
        D3D12_VERTEX_BUFFER_VIEW vb[16];
        UINT vbCount;
        D3D12_INDEX_BUFFER_VIEW ib;
        bool indexed;
        UINT count, instances, start, startInstance;
        INT base;
        D3D12_GPU_VIRTUAL_ADDRESS cb[kRootCbSlots[kVS]];
        bool foliage; // the z-prepass pixel shader discards (alpha test): leaves, grass
        bool dissolve; // it discards for the LOD dissolve only (no diffuse map): solid
        bool animated; // skinned, per-instance streams, vertices written by the CPU, an unreadable object matrix: every time
        uint64_t key;  // what it puts into the grid: shader, buffers, range, the object's matrix in the world
        const VS12* vs;                   // its outputs (the material pixel shader's input)
        int texture;                      // the z-prepass PS's first 2D texture slot, -1 none
        D3D12_GPU_DESCRIPTOR_HANDLE psSrv; // that PS's texture table as applied
        // The shared default-heap pages of its vertex and index buffers: the replay moves them to GENERIC_READ like a
        // normal draw (MarkPageRead). Bound without that, a page was promoted implicitly and a later copy into it in
        // the same list assumed COMMON; or the replay drew from a page left in COPY_DEST (D3D12 debug layer, 2026-11-11).
        // Dedicated pages are skipped: they are deleted with their buffer, which may happen before the replay.
        BufferPage* pages[17];
        UINT pageCount;
        // The main list it was recorded in. Its constants, dynamic vertices and texture table live in rings that free a
        // list's part once the GPU finished that list; a ring running full submits, waits and frees mid-frame. The replay
        // skips a draw whose list is done: its data may already be overwritten (GPU hangs in dense places, 2026-11-12:
        // only with the voxel geometry on, 5 of 8 runs at village's start).
        uint64_t serial;
    };
    struct VoxCameraPatch
    {
        int slot = -1;
        UINT offset = 0; // floats
        bool zero = false, transposed = false; // the camera-relative matrix (view without translation); stored transposed
    };
    struct VoxCameraCopy
    {
        std::vector<uint8_t> bytes;
        std::vector<VoxCameraPatch> patches;
        // Shadow maps: their vertex shader divides the depth row by the light frustum's far distance (cb5
        // PER_SHADOWGEN, GS_VS_FrustrumInfo.y: o0.z = (row 2 . p) / y * w), so the orthographic depth row is multiplied
        // by it. Without it every shadow draw landed in the first slice, which is left out: no geometry from the shadow
        // maps at all (a capture's vertex shader showed it).
        float zScale = 1.0f;
    };

    // A z-prepass draw of the scene: before this frame's GI pass, into the scene's depth with the R32F target.
    // (Measured on the gitest demo: ~750 such draws a frame, rtv0 R32_FLOAT with the scene's depth.)
    bool VoxRecording() const
    {
        return Gi().on && Gi().geometry && m_giInScene && m_giLastRun != m_giFrame && m_dsv && m_dsv->info == m_giSceneDepth && m_rtv[0] &&
               m_rtv[0]->format == DXGI_FORMAT_R32_FLOAT;
    }

    static bool MatrixNear(const float* data, const float* m, bool transposed)
    {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
            {
                const float a = data[r * 4 + c], b = transposed ? m[c * 4 + r] : m[r * 4 + c];
                const float d = a > b ? a - b : b - a, s = b > 0 ? b : -b;
                if (!(d <= 0.01f * (s > 1 ? s : 1))) // the engine's has the DLSS jitter, ours not
                    return false;
            }
        return true;
    }

    // Where the bound vertex shader reads this frame's view-projection: its constant buffers searched for the matrix and
    // the camera-relative one (the view without translation), as stored or transposed. None: the shader takes its
    // matrices per object (composite) or another camera (the weapon); it is not voxelized.
    std::vector<VoxCameraPatch> FindCameraMatrices() const
    {
        float vp[16], viewZero[16], vpZero[16];
        MatMul(m_giView, m_giProj, vp);
        memcpy(viewZero, m_giView, sizeof(viewZero));
        viewZero[12] = viewZero[13] = viewZero[14] = 0;
        MatMul(viewZero, m_giProj, vpZero);
        return FindMatrices(vp, vpZero);
    }

    // A shadow map's draws: the light's view-projection (the renderer's modelview, its projection is the identity),
    // and the same for positions relative to the camera ((p - eye) * vpZero = p * vp).
    std::vector<VoxCameraPatch> FindShadowMatrices() const
    {
        float toWorld[16], vpZero[16];
        RigidInverse(m_giView, toWorld);
        memcpy(vpZero, m_shadowMv, sizeof(vpZero));
        for (int j = 0; j < 4; j++)
            vpZero[12 + j] += toWorld[12] * m_shadowMv[j] + toWorld[13] * m_shadowMv[4 + j] + toWorld[14] * m_shadowMv[8 + j];
        return FindMatrices(m_shadowMv, vpZero);
    }

    // The engine cube's face draws: the face camera's view-projection (the renderer's modelview and projection, which the
    // stream sends before the draws), and the same for positions relative to the face's eye (the cube sits at the eye).
    std::vector<VoxCameraPatch> FindCubeMatrices() const
    {
        float vp[16], viewZero[16], vpZero[16];
        MatMul(m_passMv, m_passProj, vp);
        memcpy(viewZero, m_passMv, sizeof(viewZero));
        viewZero[12] = viewZero[13] = viewZero[14] = 0;
        MatMul(viewZero, m_passProj, vpZero);
        return FindMatrices(vp, vpZero);
    }

    // Between CryMT's "CryMT engine cube" marks (the engine drawing a face of its cube).
    bool CubeRecording() const
    {
        return m_engineCubePass && Gi().on && Gi().geometry && (Gi().cubeGeometry || Gi().cubeInject) && m_giHaveCamera && m_voxOcc[0];
    }

    std::vector<VoxCameraPatch> FindMatrices(const float* vp, const float* vpZero) const
    {
        std::vector<VoxCameraPatch> found;
        const StageState& st = m_stage[kVS];
        for (UINT slot = 0; slot < kRootCbSlots[kVS]; slot++)
        {
            const Buffer12* b = (m_vs->cbMask >> slot & 1) ? st.cb[slot] : nullptr;
            if (!b || !b->Dynamic() || !b->slice.cpu)
                continue;
            const auto* f = reinterpret_cast<const float*>(b->slice.cpu);
            for (UINT o = 0; o + 16 <= b->desc.ByteWidth / 4; o += 4)
                for (int k = 0; k < 4; k++)
                    if (MatrixNear(f + o, (k & 2) ? vpZero : vp, k & 1))
                    {
                        found.push_back({static_cast<int>(slot), o, (k & 2) != 0, (k & 1) != 0});
                        break;
                    }
        }
        return found;
    }

    int m_zDraws = 0, m_firstWaterZ = -1; // this frame's z-prepass draws seen by RecordVoxDraw, the first ocean one among them
    uint64_t m_voxDrawsFrame = ~0ull;     // the frame (g_presented) whose z-prepass draws m_voxDraws holds

    // kind: 0 the z-prepass, 1 a shadow map (CRYMT_GI_SHADOWGEOM), 2 the engine cube's face (CRYMT_GI_CUBEGEOM).
    void RecordVoxDraw(int kind, bool indexed, UINT count, UINT instances, UINT start, INT base, UINT startInstance)
    {
        const bool shadow = kind == 1, cube = kind == 2;
        if (cube)
        {
            // The face's own targets and camera, at its first scene draw (depth written into an HDR target): what the
            // inject pass reads at the face's end.
            if (!m_cubeFaceNoted && m_dsv && m_dsv->info && m_rtv[0] && m_rtv[0]->info && m_ps &&
                m_rtv[0]->format == DXGI_FORMAT_R16G16B16A16_FLOAT && !(m_depth && m_depth->d12.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO))
            {
                m_cubeFaceNoted = true;
                m_cubeFaceDepth = m_dsv->info;
                m_cubeFaceDepthFormat = m_dsv->format;
                m_cubeFaceColour = m_rtv[0]->info;
                MatMul(m_passMv, m_passProj, m_cubeFaceVp);
                memcpy(m_cubeFaceProj, m_passProj, sizeof(m_cubeFaceProj));
            }
            if (!Gi().cubeGeometry)
                return;
            m_stats.voxCubeSeen++;
            if (m_cubeDrawsFrame != g_presented) // a frame's face draws the voxelization did not take (GI voxels off)
            {
                m_cubeDrawsFrame = g_presented;
                m_cubeDraws.clear();
                m_cubeCopies.clear();
                m_cubeKeys.clear();
            }
        }
        if (kind == 0 && m_vs)
        {
            m_zDraws++;
            if (m_vs->water && m_firstWaterZ < 0)
                m_firstWaterZ = m_zDraws;
        }
        // Draws that leave no depth are no geometry: the sky's z-prepass draw (Sky.cfx ZPass: no pixel shader, depth
        // writes off) is a box around the camera and stood in the voxels as one (the user: a cube like a sky dome in
        // the Num2 view once voxel geometry was on by default).
        if ((m_depth && m_depth->d12.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO) || (!shadow && !m_ps) || (cube && !m_depth))
        {
            m_stats.voxGeomSkipped++;
            return;
        }
        if (m_gs || m_vs->srvEnd > 0 || !m_layout || m_layout->slotEnd > 16)
        {
            m_stats.voxGeomSkipped++;
            return;
        }
        // The first-person view (weapon, hands: their own depth range under kWeaponDepth) is not the world's geometry.
        if (kind == 0 && m_numViewports > 0 && m_viewports[0].MaxDepth < 0.5f)
        {
            m_stats.voxGeomWeapon++;
            return;
        }
        // The ocean's surface (its waves) is no geometry for the GI: the user saw voxels on the waves.
        if (m_vs->water)
        {
            m_stats.voxGeomSkipped++;
            return;
        }
        auto& cameras = shadow ? m_shadowCamera : cube ? m_cubeCamera : m_voxCamera;
        // Alpha-tested materials (the z-prepass pixel shader discards): leaves, grass. With their texture (the material
        // pass) they are voxelized by the shape of the leaves; CRYMT_GI_GEOMETRY=2 keeps them also without one.
        // Alpha-tested: the z-prepass discards by the diffuse map's alpha (CommonZPass.cfi %_RT_ALPHATEST). Its LOD
        // dissolve (%_RT_DISSOLVE, on most objects: clip by a noise texture) discards too, and was taken for foliage:
        // the ground and most solid objects went in half open and cut by their diffuse map's alpha (gloss there), the
        // user's holes across roads and dark spots (the capture: every level-0 stamp had foliage's opacity).
        const bool foliage = m_ps && m_ps->discards && m_ps->diffuseSlot >= 0;
        // The cube's foliage stays out: the cells it missed off screen were solid (mine, 2026-11-13: stamps 255).
        if (cube && foliage)
        {
            m_stats.voxCubeFoliage++;
            return;
        }
        if (kind == 0)
            m_stats.voxClass[(m_ps && m_ps->discards ? 2 : 0) + (m_ps && m_ps->diffuseSlot >= 0 ? 1 : 0)]++;
        auto it = cameras.find(m_vs->contentHash);
        if (it == cameras.end())
        {
            std::vector<VoxCameraPatch> found = shadow ? FindShadowMatrices() : cube ? FindCubeMatrices() : FindCameraMatrices();
            if (cube && found.empty()) // not kept: the face's matrices may not have come yet for this shader's first draw
            {
                m_stats.voxCubeNoCamera++;
                return;
            }
            it = cameras.emplace(m_vs->contentHash, std::move(found)).first;
        }
        if (kind == 0 && !foliage)
            m_stats.voxPatch[it->second.empty() ? 0 : 1 + (it->second[0].zero ? 2 : 0) + (it->second[0].transposed ? 1 : 0)]++;
        if (it->second.empty())
        {
            (shadow ? m_stats.voxShadowNoCamera : cube ? m_stats.voxCubeNoCamera : m_stats.voxGeomNoCamera)++;
            return;
        }
        VoxDraw d{};
        d.serial = g_mainSerial;
        d.vsCode = &m_vs->code;
        d.vsHash = m_vs->contentHash;
        d.layout = m_layout;
        d.topology = m_topology;
        d.vbCount = m_layout->slotEnd;
        memcpy(d.vb, m_vbApplied, d.vbCount * sizeof(D3D12_VERTEX_BUFFER_VIEW));
        d.ib = m_ibApplied;
        d.indexed = indexed;
        auto notePage = [&d](Buffer12* b) {
            BufferPage* p = b ? b->Page() : nullptr;
            if (!p || p->dedicated)
                return;
            for (UINT k = 0; k < d.pageCount; k++)
                if (d.pages[k] == p)
                    return;
            if (d.pageCount < std::size(d.pages))
                d.pages[d.pageCount++] = p;
        };
        for (UINT i = 0; i < d.vbCount; i++)
            notePage(m_vb[i].buffer);
        if (indexed)
            notePage(m_ib);
        d.count = count;
        d.instances = instances;
        d.start = start;
        d.base = base;
        d.startInstance = startInstance;
        d.foliage = foliage;
        d.dissolve = m_ps && m_ps->discards && !foliage;
        d.vs = m_vs;
        // The material's colour from its diffuse map (the dissolve variant reads only its noise texture: none then).
        d.texture = m_ps && m_ps->diffuseSlot >= 0 && m_stage[kPS].srvTable.ptr ? m_ps->diffuseSlot : -1;
        d.psSrv = m_stage[kPS].srvTable;
        const StageState& st = m_stage[kVS];
        memcpy(d.cb, st.cbApplied, sizeof(d.cb));
        // Its identity, for voxelizing unchanged objects only now and then: the shader, the buffers and the range, and the
        // object's matrix (PI_ObjMatrix, 3x4 at c0 of VS slot 3) with the translation in the world (the camera-relative
        // matrices moved back by the eye), 2 cm / 0.001 steps. A spinning radar, a door, a falling piece: a new matrix every
        // frame, a new key. Skinned characters, foliage the player bends, cloth: bones, per-instance or CPU-written vertex
        // streams, so animated (every time).
        d.animated = m_layout->animated;
        if (d.animated)
            m_stats.voxAnimatedWhy[0]++;
        for (UINT i = 0; i < d.vbCount && !d.animated; i++)
            if (m_vb[i].buffer && m_vb[i].buffer->Dynamic())
            {
                d.animated = true;
                m_stats.voxAnimatedWhy[1]++;
            }
        uint64_t key = HashWords(&d.vsHash, sizeof(d.vsHash), 1469598103934665603ull);
        key = HashWords(d.vb, d.vbCount * sizeof(D3D12_VERTEX_BUFFER_VIEW), key);
        if (indexed)
            key = HashWords(&d.ib, sizeof(d.ib), key);
        const UINT range[6] = {count, instances, start, static_cast<UINT>(base), startInstance, 0};
        key = HashWords(range, sizeof(range), key);
        if (m_vs->cbMask >> 3 & 1)
        {
            const Buffer12* object = st.cb[3];
            const bool dyn = object && object->Dynamic() && object->slice.cpu && object->desc.ByteWidth >= 48;
            if (dyn || (object && object->headKnown))
            {
                float m[12];
                memcpy(m, dyn ? static_cast<const void*>(object->slice.cpu) : static_cast<const void*>(object->head), sizeof(m));
                if (it->second[0].zero)
                {
                    float eye[16];
                    RigidInverse(m_giView, eye);
                    m[3] += eye[12];
                    m[7] += eye[13];
                    m[11] += eye[14];
                }
                int32_t q[12];
                for (int i = 0; i < 12; i++)
                {
                    const float x = m[i] * ((i & 3) == 3 ? 50.0f : 1000.0f);
                    q[i] = x > -2e9f && x < 2e9f ? static_cast<int32_t>(x + (x >= 0 ? 0.5f : -0.5f)) : 0x7FFFFFFF;
                }
                key = HashWords(q, sizeof(q), key);
            }
            else if (!d.animated)
            {
                d.animated = true;
                m_stats.voxAnimatedWhy[object ? 3 : 2]++;
            }
        }
        d.key = key;
        // A face draws an object in several passes (lights, layers): once is enough.
        if (cube && !m_cubeKeys.insert(key).second)
        {
            m_stats.voxCubeRepeat++;
            return;
        }
        // The camera buffers' contents now (the replay patches the matrices in): once per buffer and frame.
        for (const VoxCameraPatch& p : it->second)
        {
            const Buffer12* b = st.cb[p.slot];
            if (!b || !b->slice.cpu || b->Gpu() != d.cb[p.slot])
            {
                m_stats.voxGeomSkipped++;
                return;
            }
            VoxCameraCopy& copy = (shadow ? m_shadowCopies : cube ? m_cubeCopies : m_voxCameraCopies)[d.cb[p.slot]];
            if (copy.bytes.empty())
            {
                copy.bytes.assign(b->slice.cpu, b->slice.cpu + b->desc.ByteWidth);
                const Buffer12* frustum = st.cb[5];
                if (shadow && (m_vs->cbMask >> 5 & 1) && frustum && frustum->Dynamic() && frustum->slice.cpu)
                {
                    const float lightFar = reinterpret_cast<const float*>(frustum->slice.cpu)[1];
                    if (lightFar > 1.0f && lightFar < 1e9f)
                        copy.zScale = lightFar;
                }
            }
            bool known = false;
            for (const VoxCameraPatch& q : copy.patches)
                known = known || (q.offset == p.offset && q.zero == p.zero);
            if (!known)
                copy.patches.push_back(p);
        }
        if (cube && m_cubeDraws.empty())
            RigidInverse(m_passMv, m_cubeToWorld); // the face's eye and axes (its grid and camera-relative positions)
        (shadow ? m_shadowDraws : cube ? m_cubeDraws : m_voxDraws).push_back(d);
        if (shadow)
            m_stats.voxShadowRecorded++;
        else if (cube)
            m_stats.voxCubeRecorded++;
    }

    // ---- GI geometry from the shadow maps (CRYMT_GI_SHADOWGEOM): the light passes draw the casters around the player,
    // behind the camera too. The renderer's matrices come in stream order before each draw that follows a change
    // (kOpMatrices); a shadow map (vf207, Prep_PrepareDepthMap) puts the light's view-projection on the modelview stack
    // and the identity as the projection. Each frame one job: a grid level (0..2) along one axis, all nine in nine
    // frames, from the smallest of last frame's maps that covers the level around the eye (else the widest). Cells keep
    // their opacity between jobs (the update lowers it only where the screen sees through), so static geometry stays in
    // the grid while moving objects come from the z-prepass every frame.
    // The sun's shadows in the fog: at a shadow map's first draw after its matrix came (PassMatrices) the map, its light
    // matrix and far distance (the shadow vertex shaders' cb5 [1], GS_VS_FrustrumInfo.y: they write the depth as the
    // light-space z over it) go into m_sunMaps by map. Only single-sample D32 maps (the cascades; the +-2 km map is D24S8);
    // a map drawn for another light leaves the table.
    void NoteSunMap()
    {
        m_vfogShadowPending = false;
        if (!m_dsv || !m_dsv->info || m_dsv->info->buffer)
            return;
        const auto* t = static_cast<const TextureCore*>(m_dsv->info);
        int slot = -1, oldest = 0;
        for (int i = 0; i < kSunMaps; i++)
        {
            if (m_sunMaps[i].map == m_dsv->info)
                slot = i;
            if (m_sunMaps[i].frame < m_sunMaps[oldest].frame)
                oldest = i;
        }
        float lightFar = 0;
        const Buffer12* f = m_vs && (m_vs->cbMask >> 5 & 1) ? m_stage[kVS].cb[5] : nullptr;
        if (f && f->Dynamic() && f->slice.cpu && f->desc.ByteWidth >= 8)
            lightFar = reinterpret_cast<const float*>(f->slice.cpu)[1];
        const bool sun = m_vfogPendingSun && t->samples == 1 && (t->format == DXGI_FORMAT_R32_TYPELESS || t->format == DXGI_FORMAT_D32_FLOAT) &&
                         lightFar > 1.0f && lightFar < 1e9f;
        if (!sun)
        {
            if (slot >= 0)
                m_sunMaps[slot] = SunMap{};
            return;
        }
        SunMap& s = m_sunMaps[slot >= 0 ? slot : oldest];
        s.map = m_dsv->info;
        memcpy(s.mv, m_vfogPendingMv, sizeof(s.mv));
        s.lightFar = lightFar;
        s.frame = m_frame;
    }

    bool ShadowRecording()
    {
        if (m_vfogShadowPending)
            NoteSunMap();
        if (m_shadowCurrent >= 0 && m_shadowCurrent < 8)
            m_stats.voxShadowMapDraws[m_shadowCurrent]++;
        return m_shadowRecording && m_dsv && m_dsv->info != m_giSceneDepth;
    }

    // Half the width a light matrix covers around the eye, less the eye's offset from its centre (m; 0: not around it).
    float ShadowCover(const float* m, const float* eye) const
    {
        float c[4];
        for (int j = 0; j < 4; j++)
            c[j] = eye[0] * m[j] + eye[1] * m[4 + j] + eye[2] * m[8 + j] + m[12 + j];
        if (!(c[3] > 0))
            return 0;
        const float ex = std::fabs(c[0] / c[3]), ey = std::fabs(c[1] / c[3]);
        const float sx = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]), sy = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
        if (ex >= 1 || ey >= 1 || !(sx > 0) || !(sy > 0))
            return 0;
        return std::min(c[3] / sx * (1 - ex), c[3] / sy * (1 - ey));
    }

    void PassMatrices(const float* m)
    {
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(m_passMv, m, sizeof(m_passMv));
        memcpy(m_passProj, m + 16, sizeof(m_passProj));
        m_shadowRecording = false;
        m_shadowCurrent = -1;
        const bool shadow = !memcmp(m_passProj, kIdentity, sizeof(kIdentity)) && memcmp(m_passMv, kIdentity, sizeof(kIdentity));
        // The sun's shadows in the fog: a shadow map's matrix for its next draw (NoteSunMap); the sun's has w ~ 1e6 (the
        // light ~1000 km out along the sun), a lamp's a few metres.
        m_vfogShadowPending = shadow;
        m_vfogPendingSun = shadow && std::fabs(m_passMv[15]) > 1e4f;
        if (shadow)
            memcpy(m_vfogPendingMv, m_passMv, sizeof(m_vfogPendingMv));
        if (!shadow || !Gi().on || !Gi().geometry || !Gi().shadowGeometry || !m_giHaveCamera)
            return;
        if (m_shadowFrame != m_giFrame) // the frame's first map (m_giFrame advances in the GI pass)
        {
            m_shadowFrame = m_giFrame;
            m_shadowDraws.clear();
            m_shadowCopies.clear();
            m_shadowLastMaps = std::min(m_shadowMaps, 8);
            memcpy(m_shadowLastCover, m_shadowCover, sizeof(m_shadowCover));
            m_stats.voxShadowCoverCount = static_cast<uint32_t>(m_shadowLastMaps);
            memcpy(m_stats.voxShadowCover, m_shadowCover, sizeof(m_shadowCover));
            m_shadowMaps = 0;
            const int job = static_cast<int>(m_giFrame % 9);
            m_shadowLevel = job % 3;
            m_shadowAxis = job / 3;
            const float need = 0.87f * static_cast<float>(kVoxN) * Gi().voxelSize * static_cast<float>(1 << m_shadowLevel); // the level's cube seen from any side
            m_shadowPick = -1;
            for (int i = 0; i < m_shadowLastMaps; i++)
            {
                const float c = m_shadowLastCover[i], b = m_shadowPick >= 0 ? m_shadowLastCover[m_shadowPick] : -1.0f;
                if (c > 0 && (m_shadowPick < 0 || (c >= need ? (b < need || c < b) : (b < need && c > b))))
                    m_shadowPick = i;
            }
            m_stats.voxShadowJobs++;
            if (m_shadowPick >= 0 && m_shadowLastCover[m_shadowPick] >= need)
                m_stats.voxShadowCovered++;
        }
        else if (m_shadowMaps > 0 && !memcmp(m_passMv, m_shadowMv, sizeof(m_shadowMv)))
        {
            m_shadowRecording = m_shadowMaps - 1 == m_shadowPick; // back to the same map
            m_shadowCurrent = m_shadowMaps - 1;
            return;
        }
        memcpy(m_shadowMv, m_passMv, sizeof(m_shadowMv));
        float toWorld[16];
        RigidInverse(m_giView, toWorld);
        if (m_giFrame == 1000 && GetEnvironmentVariableW(L"CRYMT_SHADOWDUMP", nullptr, 0)) // diagnostics: the frame's maps
        {
            const float* x = m_passMv;
            Log("shadow map %d: eye %.2f %.2f %.2f, dsv %ux%u fmt %d, rtv %d\n  %g %g %g %g\n  %g %g %g %g\n  %g %g %g %g\n  %g %g %g %g\n", m_shadowMaps,
                toWorld[12], toWorld[13], toWorld[14], m_dsv ? static_cast<const TextureCore*>(m_dsv->info)->width : 0u,
                m_dsv ? static_cast<const TextureCore*>(m_dsv->info)->height : 0u, m_dsv ? static_cast<int>(static_cast<const TextureCore*>(m_dsv->info)->format) : -1,
                m_rtv[0] ? static_cast<int>(m_rtv[0]->format) : -1, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9], x[10], x[11], x[12], x[13], x[14], x[15]);
        }
        if (m_shadowMaps < 8)
        {
            m_shadowCover[m_shadowMaps] = ShadowCover(m_passMv, toWorld + 12);
            m_stats.voxShadowMapSeen[m_shadowMaps]++;
            m_stats.voxShadowMapCover[m_shadowMaps] += m_shadowCover[m_shadowMaps];
        }
        m_shadowRecording = m_shadowMaps == m_shadowPick;
        m_shadowCurrent = m_shadowMaps;
        m_shadowMaps++;
        m_stats.voxShadowMaps++;
    }

    bool EnsureVoxGeom()
    {
        if (m_voxGeomFailed)
            return false;
        if (m_voxOcc[0])
            return true;
        m_voxGeomFailed = true;
        using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        ID3DBlob* errors = nullptr;
        if (!compile || !g_serializeRoot ||
            FAILED(compile(kVoxGeomShader, strlen(kVoxGeomShader), "CryMT_VoxGeom", nullptr, nullptr, "voxelize", "ps_5_0", 1 << 15, 0, &m_voxGeomPs, &errors)))
        {
            Log("voxel geometry shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            return false;
        }
        // The main root signature's parameters (the engine's vertex shaders bind through them) plus the pass's own.
        static const D3D12_SHADER_VISIBILITY kVisibility[kStages] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_GEOMETRY,
                                                                     D3D12_SHADER_VISIBILITY_PIXEL};
        D3D12_ROOT_PARAMETER params[kRootParams + 2]{};
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
            // The pixel stage runs our shaders here, which sample through the static s15 alone.
            ranges[s][1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, s == kPS ? kTableSamplers - 1 : kTableSamplers, 0, 0, 0};
            params[kRootSrv[s]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kRootSrv[s]].DescriptorTable = {1, &ranges[s][0]};
            params[kRootSrv[s]].ShaderVisibility = kVisibility[s];
            params[kRootSampler[s]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kRootSampler[s]].DescriptorTable = {1, &ranges[s][1]};
            params[kRootSampler[s]].ShaderVisibility = kVisibility[s];
        }
        const D3D12_DESCRIPTOR_RANGE uav = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0}; // occupancy, albedo
        params[kVoxGeomUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kVoxGeomUav].DescriptorTable = {1, &uav};
        params[kVoxGeomUav].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[kVoxGeomConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kVoxGeomConstants].Constants = {13, 0, 8};
        params[kVoxGeomConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC material{};
        material.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        material.AddressU = material.AddressV = material.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.MaxLOD = D3D12_FLOAT32_MAX;
        material.ShaderRegister = 15;
        material.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = kRootParams + 2;
        rd.pParameters = params;
        rd.NumStaticSamplers = 1;
        rd.pStaticSamplers = &material;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
        ID3DBlob* blob = nullptr;
        if (FAILED(g_serializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&m_voxGeomRoot))))
        {
            Log("voxel geometry root signature failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        blob->Release();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        d.Width = d.Height = kVoxN;
        d.DepthOrArraySize = kVoxN;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_UINT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = S_OK;
        for (int l = 0; l < kVoxGeomLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxOcc[l]));
        d.Format = DXGI_FORMAT_R8_UINT;
        for (int l = 0; l < kVoxGeomLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxSeen[l]));
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int l = 0; l < kVoxGeomLevels && SUCCEEDED(hr); l++)
            hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&m_voxAlbedo[l]));
        m_voxCompile = reinterpret_cast<void*>(compile);
        Log("voxel geometry grids %d levels of %d^3: %s", kVoxGeomLevels, kVoxN, SUCCEEDED(hr) ? "ready" : "failed");
        m_voxGeomFailed = FAILED(hr);
        return SUCCEEDED(hr);
    }

    // The albedo grid of a level, or a null 3D view (level -1).
    void AlbedoUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int level)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(level >= 0 ? m_voxAlbedo[level] : nullptr, nullptr, &ud, at);
    }

    // The material pixel shader for a vertex shader's outputs and a texture slot: its input declares the outputs alike
    // (register order, component counts), samples the texture at the first TEXCOORD with two or more components through
    // a static linear wrap sampler (s15), drops texels under alpha 0.5 for alpha-tested materials (job.w), stamps the
    // cell as kVoxGeomShader and stores the texture's colour as the cell's albedo (u1). Null when the outputs have no
    // such coordinates or the compile fails (the draw then takes kVoxGeomShader).
    ID3DBlob* MaterialPs(const VS12* vs, int texture, int& compiled)
    {
        const uint64_t key = vs->contentHash * 131 + static_cast<uint64_t>(texture);
        auto it = m_voxMaterialPs.find(key);
        if (it != m_voxMaterialPs.end())
            return it->second;
        if (compiled >= 4 || !m_voxCompile)
            return nullptr;
        compiled++;
        std::vector<const VS12::Output*> outs;
        for (const VS12::Output& o : vs->outputs)
            outs.push_back(&o);
        std::sort(outs.begin(), outs.end(), [](const VS12::Output* a, const VS12::Output* b) {
            return a->reg != b->reg ? a->reg < b->reg : (a->mask & -a->mask) < (b->mask & -b->mask);
        });
        std::string fields, position, uv;
        for (size_t i = 0; i < outs.size(); i++)
        {
            const VS12::Output& o = *outs[i];
            const int count = __builtin_popcount(o.mask & 15);
            const char* type = o.type == D3D10_REGISTER_COMPONENT_UINT32 ? "uint" : o.type == D3D10_REGISTER_COMPONENT_SINT32 ? "int" : "float";
            char line[160];
            snprintf(line, sizeof(line), "    %s%s%d v%zu : %s%u;\n", o.type == D3D10_REGISTER_COMPONENT_FLOAT32 ? "" : "nointerpolation ", type,
                     count < 1 ? 1 : count, i, o.name.c_str(), o.index);
            fields += line;
            const bool isTexcoord = _strnicmp(o.name.c_str(), "TEXCOORD", 8) == 0;
            if (o.value == D3D10_NAME_POSITION)
                position = "i.v" + std::to_string(i);
            else if (uv.empty() && isTexcoord && count >= 2 && o.type == D3D10_REGISTER_COMPONENT_FLOAT32 && (o.mask & 3) == 3)
                uv = "i.v" + std::to_string(i) + ".xy";
        }
        ID3DBlob* blob = nullptr;
        if (!position.empty() && !uv.empty())
        {
            char head[256];
            snprintf(head, sizeof(head), "Texture2D<float4> material : register(t%d);\n", texture);
            const std::string src = std::string(kVoxMaterialHead) + head + "struct In\n{\n" + fields + "};\n" + kVoxMaterialBody1 + position +
                                    kVoxMaterialBody2 + uv + kVoxMaterialBody3;
            using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
            ID3DBlob* errors = nullptr;
            if (FAILED(reinterpret_cast<CompileFn>(m_voxCompile)(src.data(), src.size(), "CryMT_VoxMaterial", nullptr, nullptr, "voxelize", "ps_5_0", 1 << 15, 0, &blob, &errors)))
            {
                LogLimited("voxmaterial", "voxel material shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
                blob = nullptr;
            }
            if (errors)
                errors->Release();
        }
        m_voxMaterialPs.emplace(key, blob);
        if (blob)
            m_stats.voxMaterialShaders++;
        return blob;
    }

    // The seen grid of a level, or a null 3D view (level -1).
    void SeenUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int level)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8_UINT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(level >= 0 ? m_voxSeen[level] : nullptr, nullptr, &ud, at);
    }

    // The occupancy grid of a level, or a null 3D view (level -1).
    void OccupancyUav(D3D12_CPU_DESCRIPTOR_HANDLE at, int level)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.WSize = static_cast<UINT>(-1);
        g.dev->CreateUnorderedAccessView(level >= 0 ? m_voxOcc[level] : nullptr, nullptr, &ud, at);
    }

    // At most a few new pipelines a frame (created on this thread); draws without one wait for a later frame.
    ID3D12PipelineState* VoxGeomPso(const VoxDraw& d, ID3DBlob* ps, int& created)
    {
        const int type = d.topology == D3D10_PRIMITIVE_TOPOLOGY_POINTLIST                                                   ? 1
                         : d.topology == D3D10_PRIMITIVE_TOPOLOGY_LINELIST || d.topology == D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP ? 2
                         : d.topology == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST || d.topology == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ? 3
                                                                                                                                 : 0;
        if (!type)
            return nullptr;
        const uint64_t key = HashBytes(&d.layout->contentHash, sizeof(uint64_t), d.vsHash * 31 + static_cast<uint64_t>(type)) ^
                             reinterpret_cast<uintptr_t>(ps) * 0x9E3779B97F4A7C15ull;
        auto it = m_voxGeomPsos.find(key);
        if (it != m_voxGeomPsos.end())
            return it->second;
        if (created >= 8)
            return nullptr;
        created++;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = m_voxGeomRoot;
        p.VS = {d.vsCode->data(), d.vsCode->size()};
        p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.InputLayout = {d.layout->elements.data(), static_cast<UINT>(d.layout->elements.size())};
        p.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(type);
        p.SampleDesc = {1, 0};
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(g.dev->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso))))
            pso = nullptr; // e.g. a vertex shader whose position is not its first output: kept as null, not tried again
        m_voxGeomPsos.emplace(key, pso);
        m_stats.voxGeomPsos++;
        return pso;
    }

    // Inside RunGi, before the grid update: the recorded z-prepass draws into the occupancy grids of levels 0-2 (their cells,
    // toroidal), projected along x, y or z (one axis a frame, the update takes stamps of the last three frames).
    // The engine cube's face just drawn into the geometry levels (kVoxShader "inject", CRYMT_GI_CUBEINJECT): at its "end"
    // mark, while its depth and HDR colour targets still hold it. Cells outside the camera's view where the face shows a
    // surface get the cube's stamp and the face's colour.
    void RunCubeInject()
    {
        ResourceInfo* depth = m_cubeFaceDepth;
        ResourceInfo* colour = m_cubeFaceColour;
        m_cubeFaceNoted = false;
        if (!Gi().on || !Gi().cubeInject || !m_voxValid || !m_voxPsoInject || !m_voxOcc[0] || !depth || !colour || !depth->res ||
            !colour->res || m_cubeFaceDepthFormat == DXGI_FORMAT_UNKNOWN)
            return;
        const D3D12_RESOURCE_DESC dd = depth->res->GetDesc(), cd = colour->res->GetDesc();
        if (dd.Width != cd.Width || dd.Height != cd.Height || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            m_stats.voxInjectSkipped++;
            return;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        VoxTable(cpu, gpu, kVoxGeomLevels);
        Require(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FlushBarriers();
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            const int b = l * static_cast<int>(kVoxTable);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DepthSrvFormat(m_cubeFaceDepthFormat);
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            g.dev->CreateShaderResourceView(depth->res, &sd, at(b));
            sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g.dev->CreateShaderResourceView(colour->res, &sd, at(b + 1));
            NullSrv(at(b + 2));
            VoxUav(at(b + 3), 0, l);
            OccupancyUav(at(b + 4), l);
            VoxUav(at(b + 5), -1);
            SeenUav(at(b + 6), l);
            AlbedoUav(at(b + 7), l);
            VoxUav(at(b + 8), -1);
            L1Uav(at(b + 9), 0, l);
            L1Uav(at(b + 10), -1);
            for (int f = 0; f < 6; f++)
            {
                SixUav(at(b + 11 + f), f, 0, l);
                SixUav(at(b + 17 + f), f, -1);
            }
        }
        // The face's depth to distance: clip.w = s z (s = +-1 the projection's handedness), d = A s + B / distance.
        const float A = m_cubeFaceProj[10], B = m_cubeFaceProj[14], s = m_cubeFaceProj[11] >= 0 ? 1.0f : -1.0f;
        m_list->SetComputeRootSignature(m_voxRoot);
        m_list->SetComputeRoot32BitConstant(3, VoxFlags(), 0);
        m_list->SetPipelineState(m_voxPsoInject);
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            float c[kVoxConstants] = {};
            VoxConstants(c, m_cubeFaceVp, m_giFullW, m_giFullH, 0.0f, l); // worldToView, invProj: the camera's
            memcpy(c + 16, m_cubeFaceVp, 16 * sizeof(float));                // worldToPrevClip: the face's view-projection
            c[32] = -A * s;                                                   // toWorldX: its depth (x, y), size
            c[33] = B;
            c[34] = static_cast<float>(dd.Width);
            c[35] = static_cast<float>(dd.Height);
            for (int i = 0; i < 3; i++)
                c[52 + i] = c[56 + i] = static_cast<float>(m_voxPrevGrid[l][i]); // the level's cells as they stand
            m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + static_cast<UINT64>(l) * kVoxTable * g.srvInc});
            m_list->Dispatch(kVoxN / 4, kVoxN / 4, kVoxN / 4);
            D3D12_RESOURCE_BARRIER uav[3] = {};
            for (D3D12_RESOURCE_BARRIER& u : uav)
                u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav[0].UAV.pResource = m_vox[l];
            uav[1].UAV.pResource = m_voxOcc[l];
            uav[2].UAV.pResource = m_voxSeen[l];
            m_list->ResourceBarrier(3, uav);
        }
        m_stats.voxInjectRuns++;
        m_psoApplied = nullptr;
    }

    // cubeOnly: the engine cube's face just drawn (its "end" mark: its draws' data is still current), toWorld its camera.
    void RunVoxelGeometry(const float* toWorld, uint32_t stamp, bool cubeOnly = false)
    {
        if (cubeOnly ? (!Gi().geometry || m_cubeDraws.empty() || !EnsureVoxGeom())
                     : (!Gi().geometry || (m_voxDraws.empty() && m_shadowDraws.empty()) || !EnsureVoxGeom()))
        {
            if (!cubeOnly)
            {
                m_voxDraws.clear();
                m_voxCameraCopies.clear();
                m_shadowDraws.clear();
                m_shadowCopies.clear();
            }
            m_cubeDraws.clear();
            m_cubeCopies.clear();
            m_cubeKeys.clear();
            return;
        }
        // Jobs: the z-prepass draws into each level updated this frame along the frame's axis; the shadow job's draws
        // into its level along its axis.
        struct Job
        {
            const std::vector<VoxDraw>* draws;
            const std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, VoxCameraCopy>* copies;
            int level, axis;
            bool shadow;
            int first[3];
            std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS> patched;
            bool cube = false;
        };
        std::vector<Job> jobs;
        if (!cubeOnly && !m_voxDraws.empty())
            for (int l = 0; l < kVoxGeomLevels; l++)
                if (VoxUpdates(l))
                    jobs.push_back({&m_voxDraws, &m_voxCameraCopies, l, static_cast<int>(m_giFrame % 3), false, {}, {}});
        if (!cubeOnly && !m_shadowDraws.empty())
            jobs.push_back({&m_shadowDraws, &m_shadowCopies, m_shadowLevel, m_shadowAxis, true, {}, {}});
        // The cube's face into every geometry level, along an axis that turns between the face's visits (a face comes
        // every 5 or 6 runs: the turn count plus its sixth).
        if (cubeOnly)
        {
            const int axis = static_cast<int>((m_cubeTurn + m_cubeTurn / 6) % 3);
            m_cubeTurn++;
            for (int l = 0; l < kVoxGeomLevels; l++)
            {
                Job job{&m_cubeDraws, &m_cubeCopies, l, axis, false, {}, {}};
                job.cube = true;
                jobs.push_back(std::move(job));
                m_stats.voxCubeJobs++;
            }
        }
        // Row-vector orthographic projections: grid axis U -> clip x, V -> clip y (down the viewport), W -> depth 0..1.
        // The camera buffers with the matrices replaced, per job, uploaded before any state is set (an upload may start
        // a new list).
        static const int kU[3] = {1, 0, 0}, kV[3] = {2, 2, 1}, kW[3] = {0, 1, 2};
        for (Job& job : jobs)
        {
            const int l = job.level, u = kU[job.axis], v = kV[job.axis], w = kW[job.axis];
            const float cell = Gi().voxelSize * static_cast<float>(1 << l), span = cell * kVoxN;
            float corner[3];
            for (int i = 0; i < 3; i++)
            {
                const float q = toWorld[12 + i] / cell;
                job.first[i] = static_cast<int>(q) - (static_cast<float>(static_cast<int>(q)) > q ? 1 : 0) - kVoxN / 2; // as RunVoxelUpdate
                corner[i] = static_cast<float>(job.first[i]) * cell;
            }
            float ortho[16] = {}, orthoZero[16];
            ortho[u * 4 + 0] = 2 / span;
            ortho[12 + 0] = -2 * corner[u] / span - 1;
            ortho[v * 4 + 1] = -2 / span;
            ortho[12 + 1] = 2 * corner[v] / span + 1;
            ortho[w * 4 + 2] = 1 / span;
            ortho[12 + 2] = -corner[w] / span;
            ortho[15] = 1;
            // For positions relative to the camera: (p - eye) * orthoZero = p * ortho.
            memcpy(orthoZero, ortho, sizeof(ortho));
            for (int j = 0; j < 4; j++)
                orthoZero[12 + j] += toWorld[12] * ortho[j] + toWorld[13] * ortho[4 + j] + toWorld[14] * ortho[8 + j];
            for (const auto& [address, copy] : *job.copies)
            {
                const UploadAlloc up = FrameUpload((copy.bytes.size() + 255) & ~size_t{255}, 256);
                if (!up.cpu)
                    continue;
                memcpy(up.cpu, copy.bytes.data(), copy.bytes.size());
                auto* f = reinterpret_cast<float*>(up.cpu);
                for (const VoxCameraPatch& p : copy.patches)
                {
                    const float* m = p.zero ? orthoZero : ortho;
                    float scaled[16];
                    if (copy.zScale != 1.0f)
                    {
                        memcpy(scaled, m, sizeof(scaled));
                        for (int i = 0; i < 4; i++)
                            scaled[i * 4 + 2] *= copy.zScale; // the depth output's column (row vectors)
                        m = scaled;
                    }
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++)
                            f[p.offset + r * 4 + c] = p.transposed ? m[c * 4 + r] : m[r * 4 + c];
                }
                job.patched[address] = up.res->GetGPUVirtualAddress() + up.offset;
            }
        }
        // (Unchanged objects every 4th frame, CRYMT_GI_GEOMCACHE, flickered - a cell shared with foliage or a moving
        // object took the newest stamp - and was removed 2026-11-15: tag experiments-2026-11-15.)
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        VoxTable(cpu, gpu);
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            OccupancyUav({cpu.ptr + 2 * l * g.srvInc}, l);
            AlbedoUav({cpu.ptr + (2 * l + 1) * g.srvInc}, l);
        }
        FlushBarriers();
        m_list->SetGraphicsRootSignature(m_voxGeomRoot);
        const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(kVoxN), static_cast<float>(kVoxN), 0, 1};
        const D3D12_RECT scissor = {0, 0, kVoxN, kVoxN};
        m_list->RSSetViewports(1, &viewport);
        m_list->RSSetScissorRects(1, &scissor);
        m_list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        int created = 0, compiled = 0;
        ID3D12PipelineState* applied = nullptr;
        for (const Job& job : jobs)
        {
            const int l = job.level;
            const bool counted = !job.shadow && !job.cube && l == 0; // the z-prepass stats once per draw
            // The cube's stamps: frame 1 (never newer than a z-prepass stamp: InterlockedMax keeps the z-prepass's) and
            // opacity byte 253 (kVoxShader update: counted only outside the camera's view).
            const float constants[8] = {static_cast<float>(job.first[0]), static_cast<float>(job.first[1]), static_cast<float>(job.first[2]),
                                        static_cast<float>(kVoxN), static_cast<float>(job.axis), job.cube ? 1.0f : static_cast<float>(stamp), 0, 0};
            m_list->SetGraphicsRoot32BitConstants(kVoxGeomConstants, 8, constants, 0);
            m_list->SetGraphicsRootDescriptorTable(kVoxGeomUav, {gpu.ptr + 2 * l * g.srvInc});
            // Solid geometry first, then foliage, each under its own name (captures tell them apart).
            for (int part = 0; part < 2; part++)
            {
            if (job.cube && part == 1)
                continue; // no foliage from the cube
            PassName(job.cube ? "CryMT GI geometry: cube solid"
                     : job.shadow ? (part ? "CryMT GI geometry: shadow foliage" : "CryMT GI geometry: shadow solid")
                                  : (part ? "CryMT GI geometry: foliage" : "CryMT GI geometry: solid"));
            for (size_t di = 0; di < job.draws->size(); di++)
            {
                const VoxDraw& d = (*job.draws)[di];
                if (d.foliage != (part == 1))
                    continue;
                if (d.serial != g_mainSerial && SerialDone(d.serial)) // its list is done: its ring data may be reused
                {
                    m_stats.voxGeomStale++;
                    continue;
                }
                // The material shader where the z-prepass PS reads a texture; else the plain one, which alpha-tested
                // materials only get with CRYMT_GI_GEOMETRY=2 (whole cards would fill the cells).
                ID3DBlob* ps = d.texture >= 0 && Gi().materials ? MaterialPs(d.vs, d.texture, compiled) : nullptr;
                if (!ps && d.foliage && !Gi().geometryFoliage)
                {
                    if (counted)
                        m_stats.voxGeomFoliage++;
                    continue;
                }
                ID3D12PipelineState* pso = VoxGeomPso(d, ps ? ps : m_voxGeomPs, created);
                if (!pso && ps)
                    pso = VoxGeomPso(d, ps = m_voxGeomPs, created); // the outputs did not link: plain
                if (!pso)
                {
                    m_stats.voxGeomNoPso++;
                    continue;
                }
                if (pso != applied)
                    m_list->SetPipelineState(applied = pso);
                if (ps != m_voxGeomPs)
                {
                    m_list->SetGraphicsRootDescriptorTable(kRootSrv[kPS], d.psSrv);
                    if (counted)
                        m_stats.voxGeomMaterial++;
                }
                // Opacity: foliage CRYMT_GI_FOLIAGE, solid 1 (254 / 255 with the LOD dissolve: a mark in captures).
                const float job2[2] = {job.cube ? 253.5f / 255.0f : d.foliage ? Gi().foliageOpacity : d.dissolve ? 254.0f / 255.0f : 1.0f,
                                       d.foliage ? 1.0f : 0.0f};
                m_list->SetGraphicsRoot32BitConstants(kVoxGeomConstants, 2, job2, 6);
                for (UINT k = 0; k < d.pageCount; k++)
                    MarkPageRead(d.pages[k]);
                FlushBarriers();
                m_list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(d.topology));
                m_list->IASetVertexBuffers(0, d.vbCount, d.vb);
                if (d.indexed)
                    m_list->IASetIndexBuffer(&d.ib);
                for (UINT i = 0; i < kRootCbSlots[kVS]; i++)
                    if (d.cb[i])
                    {
                        const auto p = job.patched.find(d.cb[i]);
                        m_list->SetGraphicsRootConstantBufferView(kRootCb[kVS] + i, p != job.patched.end() ? p->second : d.cb[i]);
                    }
                if (d.indexed)
                    m_list->DrawIndexedInstanced(d.count, d.instances, d.start, d.base, d.startInstance);
                else
                    m_list->DrawInstanced(d.count, d.instances, d.start, d.startInstance);
                if (counted)
                    m_stats.voxGeomDraws++;
                else if (job.shadow)
                    m_stats.voxShadowDraws++;
                else if (job.cube)
                    m_stats.voxCubeDraws++;
                if (counted && !d.foliage)
                    m_stats.voxGeomSolid++;
            }
            PassNameEnd();
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = m_voxOcc[l];
            m_list->ResourceBarrier(1, &barrier);
        }
        if (!cubeOnly)
        {
            m_voxDraws.clear();
            m_voxCameraCopies.clear();
            m_shadowDraws.clear();
            m_shadowCopies.clear();
        }
        m_cubeDraws.clear();
        m_cubeCopies.clear();
        m_cubeKeys.clear();
        m_psoApplied = nullptr; // RunGi ends with RestoreListState (the cube's face: its "end" mark calls it)
    }

    // Inside RunGi: this frame's depth and the previous frame's colour into the grid.
    void RunVoxelUpdate(TextureCore* depth, const D3D12_SHADER_RESOURCE_VIEW_DESC& depthSrv, const float* toWorld, const float* reproj)
    {
        if (!EnsureVox())
        {
            GiStamp(4);
            return;
        }
        if (Gi().dirLight == 2)
            EnsureVoxSix();
        // Geometry stamps (frames + 1, kept within float precision for the constants); the update takes the last three.
        const uint32_t stamp = static_cast<uint32_t>(m_giFrame & 0xFFFFFF) + 1;
        RunVoxelGeometry(toWorld, stamp);
        GiStamp(4);
        const bool geometry = Gi().geometry && m_voxOcc[0];
        // The lamps for the light pass (b1), uploaded before any state is set (an upload may start a new list).
        D3D12_GPU_VIRTUAL_ADDRESS lamps = 0;
        const uint32_t lampCount = Gi().lampLight > 0 ? m_giLampCount : 0;
        if (geometry)
        {
            const UploadAlloc up = FrameUpload(512, 256);
            if (up.cpu)
            {
                auto* f = reinterpret_cast<float*>(up.cpu);
                memset(f, 0, 512);
                for (uint32_t k = 0; k < lampCount; k++)
                {
                    const float* s = m_giLamps[k];
                    memcpy(f + k * 4, s, 4 * sizeof(float)); // position, radius
                    for (int i = 0; i < 3; i++)
                        f[32 + k * 4 + i] = s[4 + i] * Gi().lampLight; // colour
                    f[32 + k * 4 + 3] = s[7];                          // cosine of the half angle
                    memcpy(f + 64 + k * 4, s + 8, 3 * sizeof(float));  // direction
                }
                f[96] = static_cast<float>(lampCount);
                lamps = up.res->GetGPUVirtualAddress() + up.offset;
            }
        }
        // Level 0 every frame, level 1 every 2nd, level 2 every 4th, level 3 every 8th (the first frames: all of them).
        bool update[kVoxLevels] = {VoxUpdates(0), VoxUpdates(1), VoxUpdates(2), VoxUpdates(3)};
        UINT tables = 0;
        for (int l = 0; l < kVoxLevels; l++)
            tables += update[l] ? kVoxMips : 0; // the update, then one table per mip (u0 the level below, u2 the level)
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        VoxTable(cpu, gpu, tables);
        const auto at = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + i * g.srvInc}; };
        D3D12_UNORDERED_ACCESS_VIEW_DESC nu{};
        nu.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        FlushBarriers();
        m_list->SetComputeRootSignature(m_voxRoot);
        m_list->SetComputeRoot32BitConstant(3, VoxFlags(), 0);
        UINT table = 0;
        for (int l = 0; l < kVoxLevels; l++)
        {
            if (!update[l])
                continue;
            const float vs = Gi().voxelSize * static_cast<float>(1 << l);
            for (int i = 0; i < 3; i++)
            {
                const float q = toWorld[12 + i] / vs;
                const int cell = static_cast<int>(q) - (static_cast<float>(static_cast<int>(q)) > q ? 1 : 0); // floor
                m_voxGrid[l][i] = cell - kVoxN / 2;
            }
            const UINT first = table;
            for (UINT t = 0; t < kVoxMips; t++, table++)
            {
                const int b = static_cast<int>(table * kVoxTable);
                if (t == 0)
                {
                    g.dev->CreateShaderResourceView(depth->res, &depthSrv, at(b));
                    g.dev->CreateShaderResourceView(m_giColor, nullptr, at(b + 1));
                }
                else
                {
                    NullSrv(at(b));
                    NullSrv(at(b + 1));
                }
                // t2: the foliage mask for the update (leaves are not walls; nothing filled behind them), when it fits.
                if (t == 0 && m_foliageSrv && m_foliageW == m_giFullW && m_foliageH == m_giFullH)
                    g.dev->CreateShaderResourceView(m_foliageSrv->info->res, nullptr, at(b + 2));
                else
                    NullSrv(at(b + 2));
                VoxUav(at(b + 3), t == 0 ? 0 : static_cast<int>(t) - 1, l);
                if (t == 0)
                    OccupancyUav(at(b + 4), geometry && l < kVoxGeomLevels ? l : -1); // the update's u1 (a null 3D view without geometry)
                else
                    g.dev->CreateUnorderedAccessView(nullptr, nullptr, &nu, at(b + 4));
                VoxUav(at(b + 5), t == 0 ? -1 : static_cast<int>(t), l);
                if (t == 0)
                    SeenUav(at(b + 6), geometry && l < kVoxGeomLevels ? l : -1); // the update's and light's u3
                else
                    DirUav(at(b + 6), t >= 2 ? static_cast<int>(t) - 2 : -1, l); // the downsample's dirSrc
                if (t == 0)
                    AlbedoUav(at(b + 7), geometry && l < kVoxGeomLevels ? l : -1); // the light's u4
                else
                    DirUav(at(b + 7), static_cast<int>(t) - 1, l); // the downsample's dirDst
                if (t == 0 && l == 0)
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC rd{};
                    rd.Format = DXGI_FORMAT_R8_UNORM;
                    rd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    rd.Texture3D.WSize = static_cast<UINT>(-1);
                    g.dev->CreateUnorderedAccessView(m_voxRoof, nullptr, &rd, at(b + 8)); // the roof pass's u5
                }
                else
                    VoxUav(at(b + 8), -1);
                L1Uav(at(b + 9), t == 0 ? 0 : static_cast<int>(t) - 1, l); // u6: the level's light direction (downsample: the mip below)
                L1Uav(at(b + 10), t == 0 ? -1 : static_cast<int>(t), l);   // u7: the downsample's mip
                for (int f = 0; f < 6; f++)
                {
                    SixUav(at(b + 11 + f), f, t == 0 ? 0 : static_cast<int>(t) - 1, l); // u8..u13: the six directions (the mip below)
                    SixUav(at(b + 17 + f), f, t == 0 ? -1 : static_cast<int>(t), l);   // u14..u19: the downsample's mip
                }
            }
            float c[kVoxConstants] = {};
            VoxConstants(c, toWorld, m_giFullW, m_giFullH, 0.0f, l);
            MatMul(m_giView, reproj, c + 16); // world -> this view -> the previous clip space
            if (geometry && l < kVoxGeomLevels) // prevGridMin.w: the oldest stamp taken, the level's last three voxelizations
            {
                const uint32_t window = static_cast<uint32_t>(3 * kVoxPeriod[l] - 1);
                c[59] = static_cast<float>(stamp > window ? stamp - window : 1);
                c[51] = (static_cast<float>(stamp) + (Gi().opacityGeometry && Gi().geometry ? 0.5f : 0.0f)) * (Gi().geometryPersist ? -1.0f : 1.0f); // mode: this
                // frame's stamp, a half: the opacity from the geometry only
                                                                                             // (entered cells keep only it);
                                                                                             // negative: geometry kept
            }
            m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            m_list->SetPipelineState(m_voxPsoUpdate);
            m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + first * kVoxTable * g.srvInc});
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = m_vox[l];
            m_list->ResourceBarrier(1, &uav);
            m_list->Dispatch(kVoxN / 4, kVoxN / 4, kVoxN / 4);
            m_list->ResourceBarrier(1, &uav);
            // Every update of the geometry's levels, also without sun and lamps (a pass that ran only while a lamp was
            // in the frame's list rewrote these cells and left them as they were by turns: flicker in mine).
            if (geometry && lamps && l < kVoxGeomLevels && Gi().sunAlbedo > 0)
            {
                // Sunlight for the geometry's cells: sun direction, colour and albedo in place of worldToPrevClip;
                // the lamps in b1.
                float lc[kVoxConstants];
                memcpy(lc, c, sizeof(lc));
                const float lit = m_giSun[3] > 0 ? 1.0f : 0.0f;
                const float sun[12] = {m_giSun[0], m_giSun[1], m_giSun[2], 0, m_giSun[4] * lit, m_giSun[5] * lit, m_giSun[6] * lit, 0,
                                       Gi().sunAlbedo, Gi().skyFill, Gi().lampTest ? 1.0f : 0.0f, static_cast<float>(Gi().sunMarch)};
                memcpy(lc + 16, sun, sizeof(sun));
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, lc, 0);
                m_list->SetComputeRootConstantBufferView(2, lamps);
                m_list->SetPipelineState(m_voxPsoLight);
                m_list->Dispatch(kVoxN / 4, kVoxN / 4, kVoxN / 4);
                m_list->ResourceBarrier(1, &uav);
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            }
            if (geometry && l == 0 && m_voxPsoRoof && Gi().roofCells)
            {
                // Cover above (kVoxShader "roof"): an eighth of the texture's z slabs a frame, mode the slab.
                float rc[kVoxConstants];
                memcpy(rc, c, sizeof(rc));
                rc[51] = static_cast<float>(m_giFrame % 8);
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, rc, 0);
                m_list->SetPipelineState(m_voxPsoRoof);
                m_list->Dispatch(kVoxN / 8, kVoxN / 8, kVoxN / 64);
                D3D12_RESOURCE_BARRIER rb{};
                rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                rb.UAV.pResource = m_voxRoof;
                m_list->ResourceBarrier(1, &rb);
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, c, 0);
            }
            m_list->SetPipelineState(m_voxPsoDown);
            float dc[kVoxConstants];
            memcpy(dc, c, sizeof(dc));
            dc[3] = static_cast<float>(Gi().mipOpacity); // worldToView[0].w: the downsample's opacity rule
            for (UINT mip = 1; mip < kVoxMips; mip++)
            {
                dc[51] = static_cast<float>(mip); // mode: the mip written
                m_list->SetComputeRoot32BitConstants(0, kVoxConstants, dc, 0);
                m_list->SetComputeRootDescriptorTable(1, {gpu.ptr + (first + mip) * kVoxTable * g.srvInc});
                const UINT groups = ((kVoxN >> mip) + 3) / 4;
                m_list->Dispatch(groups, groups, groups);
                D3D12_RESOURCE_BARRIER both[2] = {uav, uav};
                both[1].UAV.pResource = m_voxDir[l];
                m_list->ResourceBarrier(2, both);
            }
            memcpy(m_voxPrevGrid[l], m_voxGrid[l], sizeof(m_voxGrid[l]));
        }
        m_voxValid = true;
        m_stats.voxPasses++;
        if (g_voxSnapWanted.load(std::memory_order_relaxed) && m_voxSnap.empty())
            RecordVoxSnap(toWorld);
    }

    static DXGI_FORMAT VoxViewFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return f;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
