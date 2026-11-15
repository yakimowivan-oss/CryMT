// CryMT D3D12 back end, class Device: the extra render targets: grass velocity, particle layer, DLSS bias mask, object motion, foliage mask.
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // The frame's main camera from the stream (kOpCamera): the scene's draws follow, the GI pass may run.
    // The frame texture (Textures/CryMT/jitter.dds's stand-in, 8x1 RGBA32F) for the patched ShadowMaskGen.cfx:
    // 0 the camera jitter (shiftU, shiftV) and the frame (0..63, 0 without jitter): the shader rebuilds a pixel's point
    // at the pixel minus the jitter, where the jittered depth's point lies; with the engine's unjittered frustum rays
    // the point left the surface by the depth gradient times up to half a pixel, and flat surfaces at grazing light
    // flipped in and out of shadow with each jitter phase (village shelves: 4366 changed pixels a frame, 6 without
    // shadows, 9 without DLAA). 1 towards the sun, w 2 with contact shadows; 2..5 the columns of the unjittered
    // view-projection (row vectors); 6 the contact shadows' length and thickness at 10 m, their reach (metres).
    // Set with the frame's main camera, before its draws.
    // The patched z-prepass (SV_Target1: 1 on alpha-tested surfaces) into the scene's R32F depth target: the foliage
    // mask bound as the second target (made at that target's size), cleared at the frame's first such draw. Targets
    // smaller than the previous frame's largest z-prepass (the reflections') are left out. That was "the largest ever"
    // until 2026-11-13: after the game's resolution went down (5120 -> 2560 wide) the scene's own z-prepass counted as a
    // reflection, so no mask, no depth for the screen-space shadows and the water's SSR for the rest of the session.
    // Wind-bent vegetation's screen move (R16G16F, the scene's size): the patched z-prepass's SV_Target2 (before minus now,
    // screen fractions per 1/60 s; 0 on everything not bent), cleared at the frame's first z-prepass draw. RunMotion
    // adds it scaled to the frame's time.
    void BindBendVelocity(UINT w, UINT h)
    {
        if (!m_bendRtv || w != m_bendW || h != m_bendH)
        {
            if (m_bendSrv)
                m_bendSrv->Release();
            if (m_bendRtv)
                m_bendRtv->Release();
            if (m_bendTex)
                m_bendTex->Release();
            m_bendSrv = nullptr;
            m_bendRtv = nullptr;
            m_bendTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16G16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("bent vegetation's motion %ux%u: failed", w, h);
                return;
            }
            m_bendTex = tex;
            m_bendRtv = static_cast<Rtv12*>(rtv);
            m_bendSrv = static_cast<Srv12*>(srv);
            m_bendW = w;
            m_bendH = h;
            m_bendFrame = ~0ull;
            Log("bent vegetation's motion %ux%u", w, h);
        }
        if (m_bendFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_bendRtv, kZero);
            m_bendFrame = m_frame;
        }
        m_bendRtv->info->outputBound++;
        Bind(m_rtv[2], m_bendRtv);
    }

    // The particle layer for the water's SSR (R16F, the scene's size; two, a frame each, so the water reads the last frame's
    // while this frame's is drawn): the patched particles' SV_Target2, the nearest kept (Pso: MIN), cleared to far (65504)
    // at the frame's first particle draw. CRYMT_SSR_PARTICLES=0: none.
    void BindParticleLayer()
    {
        static const bool on = [] {
            wchar_t v[8] = {};
            return !(GetEnvironmentVariableW(L"CRYMT_SSR_PARTICLES", v, 8) && wcscmp(v, L"0") == 0);
        }();
        if (!on || !WaterSsrOn())
            return;
        const UINT w = m_foliageW, h = m_foliageH;
        if (!m_partRtv[0] || w != m_partW || h != m_partH)
        {
            for (int i = 0; i < 2; i++)
            {
                if (m_partSrv[i])
                    m_partSrv[i]->Release();
                if (m_partRtv[i])
                    m_partRtv[i]->Release();
                if (m_partTex[i])
                    m_partTex[i]->Release();
                m_partSrv[i] = nullptr;
                m_partRtv[i] = nullptr;
                m_partTex[i] = nullptr;
                m_partFrame[i] = ~0ull;
            }
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            for (int i = 0; i < 2; i++)
            {
                ID3D10Texture2D* tex = nullptr;
                ID3D10RenderTargetView* rtv = nullptr;
                ID3D10ShaderResourceView* srv = nullptr;
                if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                    FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
                {
                    if (rtv)
                        rtv->Release();
                    if (tex)
                        tex->Release();
                    Log("particle layer %ux%u: failed", w, h);
                    return;
                }
                m_partTex[i] = tex;
                m_partRtv[i] = static_cast<Rtv12*>(rtv);
                m_partSrv[i] = static_cast<Srv12*>(srv);
            }
            m_partW = w;
            m_partH = h;
            Log("particle layer %ux%u (the water's SSR)", w, h);
        }
        const int k = static_cast<int>(m_frame & 1);
        if (m_partFrame[k] != m_frame)
        {
            static const FLOAT kFar[4] = {65504.0f, 65504.0f, 65504.0f, 65504.0f};
            ClearRenderTargetView(m_partRtv[k], kFar);
            m_partFrame[k] = m_frame;
        }
        m_partDraws++;
        m_partRtv[k]->info->outputBound++;
        Bind(m_rtv[2], m_partRtv[k]);
        m_omDirty = m_psoDirty = true;
    }

    // The last frame's particle layer, when it was drawn (the water's SSR).
    Srv12* LastParticleLayer() const
    {
        const int k = static_cast<int>((m_frame + 1) & 1);
        return m_frame > 0 && m_partSrv[k] && m_partFrame[k] == m_frame - 1 ? m_partSrv[k] : nullptr;
    }

    // DLSS's bias towards the current colour (R16F, the scene's size): the patched water and particle pixel shaders'
    // SV_Target1, blended like the colour (Pso: red only), cleared at the frame's first such draw.
    void BindBiasMask()
    {
        const UINT w = m_foliageW, h = m_foliageH;
        if (!m_biasRtv || w != m_biasW || h != m_biasH)
        {
            if (m_biasSrv)
                m_biasSrv->Release();
            if (m_biasRtv)
                m_biasRtv->Release();
            if (m_biasTex)
                m_biasTex->Release();
            m_biasSrv = nullptr;
            m_biasRtv = nullptr;
            m_biasTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("DLSS bias mask %ux%u: failed", w, h);
                return;
            }
            m_biasTex = tex;
            m_biasRtv = static_cast<Rtv12*>(rtv);
            m_biasSrv = static_cast<Srv12*>(srv);
            m_biasW = w;
            m_biasH = h;
            m_biasFrame = ~0ull;
            Log("DLSS bias mask %ux%u", w, h);
        }
        if (m_biasFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_biasRtv, kZero);
            m_biasFrame = m_frame;
            m_stats.biasFrames++;
        }
        m_stats.biasDraws++;
        m_biasRtv->info->outputBound++;
        Bind(m_rtv[1], m_biasRtv);
        m_omDirty = m_psoDirty = true;
    }

    // Exact object motion (R16G16B16A16F, the motion blur target's size): the patched motion blur pass's SV_Target1
    // (Settings.cpp kMotionBlurExact: before minus now in screen fractions, distance / far, 1 exact or 0.5 with the
    // engine's shutter factor still in; 0 where no moving object was drawn), cleared at the frame's first such draw.
    // RunMotion reads it in place of the engine's own target.
    void BindObjectMotion()
    {
        const auto* t = static_cast<const TextureCore*>(m_rtv[0]->info);
        const UINT w = t->width, h = t->height;
        if (!m_objMvRtv || w != m_objMvW || h != m_objMvH)
        {
            if (m_objMvSrv)
                m_objMvSrv->Release();
            if (m_objMvRtv)
                m_objMvRtv->Release();
            if (m_objMvTex)
                m_objMvTex->Release();
            m_objMvSrv = nullptr;
            m_objMvRtv = nullptr;
            m_objMvTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("exact object motion %ux%u: failed", w, h);
                return;
            }
            m_objMvTex = tex;
            m_objMvRtv = static_cast<Rtv12*>(rtv);
            m_objMvSrv = static_cast<Srv12*>(srv);
            m_objMvW = w;
            m_objMvH = h;
            m_objMvFrame = ~0ull;
            Log("exact object motion %ux%u", w, h);
        }
        if (m_objMvFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_objMvRtv, kZero);
            m_objMvFrame = m_frame;
            m_stats.objectMotionExactFrames++;
        }
        m_stats.objectMotionExactDraws++;
        m_objMvRtv->info->outputBound++;
        Bind(m_rtv[1], m_objMvRtv);
        m_omDirty = m_psoDirty = true;
    }

    void BindFoliageMask()
    {
        const auto* t = static_cast<const TextureCore*>(m_rtv[0]->info);
        const UINT w = t->width, h = t->height;
        if (m_zMaxFrame != m_frame)
        {
            if (m_zMaxW)
            {
                m_zPrevW = m_zMaxW;
                m_zPrevH = m_zMaxH;
            }
            m_zMaxW = m_zMaxH = 0;
            m_zMaxFrame = m_frame;
        }
        m_zMaxW = std::max(m_zMaxW, w);
        m_zMaxH = std::max(m_zMaxH, h);
        if (w < m_zPrevW || h < m_zPrevH)
            return;
        if (!m_foliageRtv || w != m_foliageW || h != m_foliageH)
        {
            if (m_foliageSrv)
                m_foliageSrv->Release();
            if (m_foliageRtv)
                m_foliageRtv->Release();
            if (m_foliageTex)
                m_foliageTex->Release();
            m_foliageSrv = nullptr;
            m_foliageRtv = nullptr;
            m_foliageTex = nullptr;
            D3D10_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D10_USAGE_DEFAULT;
            td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            ID3D10Texture2D* tex = nullptr;
            ID3D10RenderTargetView* rtv = nullptr;
            ID3D10ShaderResourceView* srv = nullptr;
            if (FAILED(CreateTexture2D(&td, nullptr, &tex)) || FAILED(CreateRenderTargetView(tex, nullptr, &rtv)) ||
                FAILED(CreateShaderResourceView(tex, nullptr, &srv)))
            {
                if (rtv)
                    rtv->Release();
                if (tex)
                    tex->Release();
                Log("foliage mask %ux%u: failed", w, h);
                return;
            }
            m_foliageTex = tex;
            m_foliageRtv = static_cast<Rtv12*>(rtv);
            m_foliageSrv = static_cast<Srv12*>(srv);
            m_foliageW = w;
            m_foliageH = h;
            m_foliageFrame = ~0ull;
            Log("foliage mask %ux%u", w, h);
        }
        if (m_foliageFrame != m_frame)
        {
            static const FLOAT kZero[4] = {};
            ClearRenderTargetView(m_foliageRtv, kZero);
            m_foliageFrame = m_frame;
            m_stats.foliageFrames++;
        }
        m_foliageRtv->info->outputBound++;
        Bind(m_rtv[1], m_foliageRtv);
        if (m_ps->writesTarget2 && !m_rtv[2])
            BindBendVelocity(w, h);
        m_omDirty = m_psoDirty = true;
        m_zTarget = static_cast<TextureCore*>(m_rtv[0]->info); // the depth for RunSss
        m_zTargetFrame = m_frame;
    }
