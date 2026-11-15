// CryMT D3D12 back end, class Device: the environment and water reflection settings, the env map substitution.
// Included inside the class in D3D12Backend.cpp, where this text stood.
    // A 4x4 texture of the CRYMT_ENVTEST colour (a cube: 6 faces).
    Srv12* EnvTestTexture(bool cube)
    {
        const uint32_t c = EnvTestColour();
        uint32_t texels[4 * 4 * 6];
        for (uint32_t& x : texels)
            x = 0xFF000000u | (c >> 16 & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16; // R8G8B8A8: R in the low byte
        D3D10_TEXTURE2D_DESC d{4, 4, 1, cube ? 6u : 1u, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE, 0,
                               cube ? static_cast<UINT>(D3D10_RESOURCE_MISC_TEXTURECUBE) : 0u};
        D3D10_SUBRESOURCE_DATA init[6];
        for (int f = 0; f < 6; f++)
            init[f] = {texels + f * 16, 16, 64};
        ID3D10Texture2D* tex = nullptr;
        ID3D10ShaderResourceView* view = nullptr;
        Srv12* out = nullptr;
        if (SUCCEEDED(CreateTexture2D(&d, init, &tex)) && SUCCEEDED(CreateShaderResourceView(tex, nullptr, &view)))
            out = static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(view));
        if (tex)
            tex->Release(); // the view keeps it
        return out;
    }

    // CRYMT_ENVMAP=voxels: the materials' environment maps from the voxel grid (with the GI's voxel part on).
    // ... or the panel's Num0 on its environment page (CRYMT_GI_KEYS).
    // The materials' reflection: 0 the game's maps (CRYMT_ENVMAP=game), 1 the voxel grid's cube (=voxels), 2 the engine's own
    // cube (=engine, the default since 2026-11-13: its HDR copy encoded as the voxel one, with mips by gloss); the page.
    static int EnvModeBase()
    {
        static const int mode = [] {
            char v[16] = {};
            if (!GetEnvironmentVariableA("CRYMT_ENVMAP", v, sizeof(v)))
                return 2;
            return _stricmp(v, "voxels") == 0 ? 1 : _stricmp(v, "engine") == 0 ? 2 : 0;
        }();
        return mode;
    }
    static int EnvMode()
    {
        const int key = g_giControl.env;
        return g_giControl.active && key >= 0 ? std::min(key, 2) : EnvModeBase();
    }
    static bool EnvOn() { return EnvMode() == 1; }       // the voxel cube (made by the GI pass)
    static bool EnvEngineOn() { return EnvMode() == 2; } // the engine's cube

    // The ocean's reflection (reflections phases 0 and 1): 0 planar, 1 the environment cube, 2 SSR + the cube;
    // CRYMT_WATERCUBE=1/2 or the reflections page.
    // Default 2 since 2026-11-13 (the user: "turn our new reflections on by default").
    static int WaterModeBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_WATERCUBE", v, 8) ? std::min(std::max(_wtoi(v), 0), 2) : 2;
        }();
        return env;
    }
    static int WaterMode()
    {
        const int panel = g_giControl.waterCube;
        return panel < 0 ? WaterModeBase() : std::min(panel, 2);
    }
    static bool WaterCubeOn() { return WaterMode() >= 1; }
    // The cube's brightness on the water (CRYMT_WATERSKY, the reflections page, crymt_watersky), 1 = as decoded. Default
    // 0.75: the low sea view against the planar reflection (2026-11-13, wsky): water luma +13% at 1.0, -2% at 0.7; the
    // cube's sky near the horizon is greyer (blue -5..-7) at any scale.
    static float WaterSkyBase()
    {
        static const float env = [] {
            wchar_t v[16] = {};
            return GetEnvironmentVariableW(L"CRYMT_WATERSKY", v, 16) ? static_cast<float>(_wtof(v)) : 0.75f;
        }();
        return env > 0 ? env : 0.75f;
    }
    static float WaterSky()
    {
        const float panel = g_giControl.waterSky;
        return panel > 0 ? panel : WaterSkyBase();
    }
    static bool WaterSsrOn() { return WaterMode() == 2; }
    // The water's cube (reflections phase 4): 1 the engine's own capture at the camera (CryMT.cpp EnvCubeUpdate: one face
    // a frame, RGBK), 0 the voxel grid's; CRYMT_ENVCUBE or the reflections page. Default the engine's.
    static bool EngineCubeOn()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE", v, 8) ? _wtoi(v) : 1;
        }();
        const int panel = g_giControl.waterSource;
        return (panel < 0 ? env : panel) == 1;
    }
    // How often the engine's cube draws a face (CryMT.cpp EnvCubeUpdate reads the panel's value): every frame (the whole
    // cube in 6 frames) to every 8th (48 frames); CRYMT_ENVCUBE_RATE, the reflections page.
    static constexpr int kEnvCubeRates[6] = {1, 2, 3, 4, 6, 8};
    static int EnvCubeRateBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_RATE", v, 8) ? std::min(std::max(_wtoi(v), 1), 8) : 1;
        }();
        return env;
    }
    static int EnvCubeRateIndex(int rate)
    {
        int i = 0;
        while (i < 5 && kEnvCubeRates[i + 1] <= rate)
            i++;
        return i;
    }
    // The culling of the engine's cube faces (in its render: m_engineCubePass): 0 as the engine sets it, 1 none, 2 the
    // other side, the default (2026-11-13: the faces lacked the near terrain and the houses, palms stayed: the face
    // cameras' images are mirrored, so the winding turns and the front faces were the ones culled; a hint the user was
    // given: "r->SetCullMode(R_CULL_NONE)"). CRYMT_ENVCUBE_CULL, the reflections page.
    static int EngineCubeCullBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_CULL", v, 8) ? std::min(std::max(_wtoi(v), 0), 3) : 3;
        }();
        return env;
    }
    static int EngineCubeCull()
    {
        const int panel = g_giControl.envCubeCull;
        return panel < 0 ? EngineCubeCullBase() : std::min(panel, 3);
    }
    // 3 (the default since 2026-11-13 night): per face of the cube (its D3D slice, +X -X +Y -Y +Z -Z). Village, each
    // slice drawn with every mode and compared with no culling (closed meshes look the same with the right one):
    // slices 0 1 2 4 5 the inverted one, slice 3 (-Y) the engine's (its image is not mirrored; the walls had holes in
    // it - the user). Island 2026-11-14 (tools\prof\cube_modes.ps1 + cube_faces_vs.py, against no culling): slice 3
    // inverted 22% of pixels off, the engine's 6%; the others inverted 1-3%, the engine's 4-10%. The table was indexed
    // by the face number of the start mark (the engine's entry +8), which is not the slice the render ends in: slice 3
    // got the inverted culling (the user: the defaults showed the cube wrong). Now by the slice that number was last
    // seen to end in (g_cubeSliceOfMark, learned at each render's end; until then the number itself).
    static int EngineCubeSlice(int markFace)
    {
        return markFace >= 0 && markFace < 6 && g_cubeSliceOfMark[markFace] >= 0 ? g_cubeSliceOfMark[markFace] : markFace;
    }
    static int EngineCubeCullFor(int markFace)
    {
        static constexpr int kPerSlice[6] = {2, 2, 2, 0, 2, 2};
        const int mode = EngineCubeCull();
        const int slice = EngineCubeSlice(markFace);
        return mode < 3 ? mode : slice >= 0 && slice < 6 ? kPerSlice[slice] : 2;
    }
    // A new face mixed into the HDR copy (CRYMT_ENVCUBE_BLEND, the reflections page): 1 replaces, 0.5 half (default), 0.25.
    static float EngineCubeBlend()
    {
        static const float env = [] {
            wchar_t v[16] = {};
            const float b = GetEnvironmentVariableW(L"CRYMT_ENVCUBE_BLEND", v, 16) ? static_cast<float>(_wtof(v)) : 0.5f;
            return b > 0.0f && b <= 1.0f ? b : 0.5f;
        }();
        const int panel = g_giControl.envCubeBlend;
        return panel < 0 ? env : panel == 0 ? 1.0f : panel == 1 ? 0.5f : 0.25f;
    }
    static int EngineCubeBlendIndex(float b) { return b >= 0.99f ? 0 : b >= 0.4f ? 1 : 2; }
    static float EngineCubeBlendBase()
    {
        const int saved = g_giControl.envCubeBlend.exchange(-1);
        const float b = EngineCubeBlend();
        g_giControl.envCubeBlend = saved;
        return b;
    }
    // The -Z face (the water's reflections look up): CRYMT_ENVCUBE_DOWN 1 draws it, default 0 (once after each start).
    static int EngineCubeDownBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_DOWN", v, 8) ? (_wtoi(v) != 0 ? 1 : 0) : 0;
        }();
        return env;
    }
    // Entities in the engine's cube (clouds, vehicles, characters): CRYMT_ENVCUBE_ENTITIES, default 1 (CryMT.cpp draws them).
    static int EngineCubeEntitiesBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_ENTITIES", v, 8) ? (_wtoi(v) != 0 ? 1 : 0) : 1;
        }();
        return env;
    }
    // The clouds in it: CRYMT_ENVCUBE_CLOUDS, default 0 (CryMT.cpp leaves them out).
    static int EngineCubeCloudsBase()
    {
        static const int env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_ENVCUBE_CLOUDS", v, 8) ? (_wtoi(v) != 0 ? 1 : 0) : 0;
        }();
        return env;
    }
    // Its brightness on the water (CRYMT_ENVCUBE_SCALE, default 1: the engine drew it as it draws the planar reflection).
    static float EngineCubeScale()
    {
        static const float env = [] {
            wchar_t v[16] = {};
            const float s = GetEnvironmentVariableW(L"CRYMT_ENVCUBE_SCALE", v, 16) ? static_cast<float>(_wtof(v)) : 1.0f;
            return s > 0 ? s : 1.0f;
        }();
        return env;
    }

    // The engine's cube as the water reads it (stream thread): a cube view of its first level (the engine draws only that)
    // made when the render thread hands over another texture; null until it has all six faces.
    Srv12* EngineCube()
    {
        ID3D10Resource* res = nullptr;
        {
            std::lock_guard<std::mutex> guard(g_engineCubeLock);
            res = g_engineCubePending;
        }
        if (res == m_engineCubeRes)
            return m_engineCube;
        if (m_engineCube)
            m_engineCube->Release();
        m_engineCube = nullptr;
        m_engineCubeRes = res;
        ID3D10Texture2D* tex = nullptr; // Dx12EngineEnvCube is given a 2D texture; asked anyway
        if (!res || FAILED(res->QueryInterface(__uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&tex))))
            return nullptr;
        D3D10_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        tex->Release();
        D3D10_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                 : td.Format;
        sd.ViewDimension = D3D10_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MostDetailedMip = 0;
        sd.TextureCube.MipLevels = 1;
        ID3D10ShaderResourceView* view = nullptr;
        if (td.ArraySize != 6 || !(td.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE) || FAILED(CreateShaderResourceView(res, &sd, &view)))
        {
            Log("engine environment cube: no view (%ux%u format %d, array %u, misc %x)", td.Width, td.Height, static_cast<int>(td.Format),
                td.ArraySize, td.MiscFlags);
            return nullptr;
        }
        m_engineCube = static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(view));
        m_engineCubeViews++;
        Log("engine environment cube for the water: %ux%u format %d, %u levels (the first read)", td.Width, td.Height,
            static_cast<int>(td.Format), td.MipLevels);
        return m_engineCube;
    }
    // The cube the water reads (Water.cfx's waterenv.dds placeholder): kind 0 the voxel grid's, 1 the engine's RGBK cube as
    // it is (the faces flipped: the shader turns its lookup), 2 the HDR copy with mips (D3D's layout), once it has all faces.
    struct EnvView
    {
        ResourceInfo* info = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        int kind = 0;
    };
    EnvView WaterEnvCube()
    {
        EnvView v;
        if (!WaterCubeOn())
            return v;
        if (EngineCubeOn())
        {
            Srv12* raw = EngineCube();
            bool all = m_hdrCube.res != nullptr;
            for (bool f : m_hdrFaceValid)
                all = all && f;
            if (raw && all)
            {
                v.info = &m_hdrCube;
                v.cpu = m_hdrCubeSrv;
                v.kind = 2;
            }
            else if (raw)
            {
                v.info = raw->info;
                v.cpu = raw->cpu;
                v.kind = 1;
            }
            return v;
        }
        if (m_envReady && m_envCubeAll)
        {
            v.info = m_envCubeAll->info;
            v.cpu = m_envCubeAll->cpu;
        }
        return v;
    }

    bool EnsureEnvTargets()
    {
        if (m_envAtlas[0] || m_envFailed)
            return m_envAtlas[0] != nullptr;
        m_envFailed = true;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = kEnvSize * 26;
        d.Height = kEnvSize;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (int k = 0; k < 2; k++)
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                      k ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_envAtlas[k]))))
            {
                Log("environment maps: no atlas");
                if (m_envAtlas[0])
                    m_envAtlas[0]->Release();
                m_envAtlas[0] = nullptr;
                return false;
            }
        for (int k = 0; k < 3; k++)
        {
            D3D10_TEXTURE2D_DESC td{kEnvSize, kEnvSize, 1, k == 1 ? 1u : 6u, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_SHADER_RESOURCE, 0,
                                    k == 1 ? 0u : static_cast<UINT>(D3D10_RESOURCE_MISC_TEXTURECUBE)};
            ID3D10Texture2D* tex = nullptr;
            ID3D10ShaderResourceView* view = nullptr;
            if (SUCCEEDED(CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(CreateShaderResourceView(tex, nullptr, &view)))
                (k == 2 ? m_envCubeAll : k ? m_env2D : m_envCube) = static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(view));
            if (tex)
                tex->Release(); // the view keeps it
        }
        if (!m_envCube || !m_env2D || !m_envCubeAll)
        {
            m_envAtlas[0]->Release();
            m_envAtlas[1]->Release();
            m_envAtlas[0] = m_envAtlas[1] = nullptr;
            Log("environment maps: no cube or 2D map");
            return false;
        }
        m_envFailed = false;
        Log("environment maps from the voxel grid: %ux%u cube and 2D map, every 4th frame", kEnvSize, kEnvSize);
        return true;
    }

    // What the material's environment map in this slot of the pixel shader is replaced by (null: kept): the CRYMT_ENVTEST
    // colour (stage 0), or the maps from the voxel grid once they have been made.
    // A material's own environment map replaced: the voxel cube / 2D map (mode 1), the engine's cube / the 2D map made from
    // it (mode 2; until 2026-11-15 the 2D maps kept the game's static one in this mode: the first-person weapons' Metal).
    // Every material alike (by its samplers' names), but water and glass (IsRefractiveResource) and eyes, hair, skin.
    EnvView EnvSubstitute(int slot)
    {
        EnvView v;
        if (!m_ps || (slot != m_ps->envCubeSlot && slot != m_ps->env2DSlot) || m_ps->refractive || m_ps->noEnv)
            return v; // water, glass: their maps stay (the user: the river came out overbright); eyes, hair, skin too
        if (EnvTestColour() == 0xFFFFFFFFu)
        {
            if (EnvEngineOn() && m_ldrCubeReady && slot == m_ps->envCubeSlot)
            {
                v.info = &m_ldrCubeOwn;
                v.cpu = m_ldrCubeOwnSrv;
            }
            else if (EnvEngineOn() && m_ldr2DReady && slot == m_ps->env2DSlot)
            {
                v.info = &m_ldr2D;
                v.cpu = m_ldr2DSrv;
            }
            else if (Srv12* s = EnvOn() && m_envReady ? (slot == m_ps->envCubeSlot ? m_envCube : m_env2D) : nullptr)
            {
                v.info = s->info;
                v.cpu = s->cpu;
            }
            return v;
        }
        if (!m_envTestTried)
        {
            m_envTestTried = true;
            m_envTestCube = EnvTestTexture(true);
            m_envTest2D = EnvTestTexture(false);
            Log("environment test textures %06x: cube %s, 2D %s", EnvTestColour(), m_envTestCube ? "ready" : "failed", m_envTest2D ? "ready" : "failed");
        }
        if (Srv12* s = slot == m_ps->envCubeSlot ? m_envTestCube : m_envTest2D)
        {
            v.info = s->info;
            v.cpu = s->cpu;
        }
        return v;
    }
