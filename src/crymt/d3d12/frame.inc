// CryMT D3D12 back end, class Device: the frame texture (water SSR previous frame), the Forward+ test, the GI camera.
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // The water's SSR reads the previous frame's finished scene (the GI's colour copy at SceneDone: after the particles and
    // clouds, which the scene copy Water.cfx reads is taken before; the user, 2026-11-15: "чтобы отражались particle и
    // облака"). Texels 8..10 (out[0..11]): this frame's homogeneous screen point s = (u w, v w, w, 1) - or a direction,
    // w 0 - to the previous frame's (u w, v w, w), each frame with its own jitter, in double; texel 11: 1 when the copy
    // is the last frame's, and its size. s = H A + t for H the point's offset from the camera (world), so H = (s - t)
    // A^-1; the previous view is (H + c - c') R', then its projection.
    void WaterPrevFrame(const DlssSceneInfo* info, float* out)
    {
        // The frame texture is updated at every main camera of a frame (6 at once seen on island): the frame's latest
        // camera is kept as this frame's, and moves to the previous one only when the frame changes (2026-11-15: each
        // later call took this frame's camera for the last one's and switched the previous frame off for the water).
        if (m_ssrCurFrame != m_frame)
        {
            if (m_ssrCurFrame != ~0ull)
            {
                memcpy(m_ssrPrevView, m_ssrCurView, sizeof(m_ssrPrevView));
                memcpy(m_ssrPrevProj, m_ssrCurProj, sizeof(m_ssrPrevProj));
                memcpy(m_ssrPrevCamPos, m_ssrCurCamPos, sizeof(m_ssrPrevCamPos));
                memcpy(m_ssrPrevShift, m_ssrCurShift, sizeof(m_ssrPrevShift));
                m_ssrPrevCamFrame = m_ssrCurFrame;
            }
            m_ssrCurFrame = m_frame;
        }
        memcpy(m_ssrCurView, info->view, sizeof(m_ssrCurView));
        memcpy(m_ssrCurProj, info->proj, sizeof(m_ssrCurProj));
        memcpy(m_ssrCurCamPos, info->camPos, sizeof(m_ssrCurCamPos));
        m_ssrCurShift[0] = info->shiftU;
        m_ssrCurShift[1] = info->shiftV;
        const bool prevCamera = m_ssrPrevCamFrame + 1 == m_frame;
        // CRYMT_SSR_PREV=0: the current scene copy only, as before (A/B).
        static const bool allowed = [] {
            wchar_t v[8] = {};
            return !(GetEnvironmentVariableW(L"CRYMT_SSR_PREV", v, 8) && wcscmp(v, L"0") == 0);
        }();
        m_ssrPrevOn = allowed && WaterSsrOn() && !m_hizFailed && m_giColor && m_giColorFrame + 1 == m_frame && prevCamera && !info->reset;
        static uint64_t logged, loggedFrame = ~0ull;
        if (logged < 3 && (m_frame % 600) == 599 && loggedFrame != m_frame)
        {
            logged++;
            loggedFrame = m_frame;
            Log("water SSR previous frame at frame %llu: on %d (allowed %d, SSR %d, pyramid failed %d, GI colour %d, its frame %llu, "
                "previous camera frame %llu, reset %u; particle layer last frame %d, draws %llu)",
                static_cast<unsigned long long>(m_frame), m_ssrPrevOn ? 1 : 0, allowed ? 1 : 0, WaterSsrOn() ? 1 : 0, m_hizFailed ? 1 : 0,
                m_giColor ? 1 : 0, static_cast<unsigned long long>(m_giColorFrame), static_cast<unsigned long long>(m_ssrPrevCamFrame),
                info->reset, LastParticleLayer() ? 1 : 0, static_cast<unsigned long long>(m_partDraws));
        }
        if (m_ssrPrevOn && m_ssrPrevRes != m_giColor)
        {
            if (!m_ssrPrevSrv.ptr)
                m_ssrPrevSrv = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_giColor, nullptr, m_ssrPrevSrv);
            m_ssrPrevRes = m_giColor;
        }
        if (m_ssrPrevOn)
        {
            // This frame: the columns of (u w, v w, w) over H (rows 0..2) and t (row 3).
            double a[4][3] = {};
            const double su = info->shiftU, sv = info->shiftV;
            for (int i = 0; i < 4; i++)
            {
                double x = 0, y = 0, w = 0;
                for (int j = 0; j < 3; j++)
                {
                    const double r = i < 3 ? static_cast<double>(info->view[i * 4 + j]) : 0.0;
                    x += r * info->proj[j * 4 + 0];
                    y += r * info->proj[j * 4 + 1];
                    w += r * info->proj[j * 4 + 3];
                }
                if (i == 3)
                {
                    x = info->proj[12];
                    y = info->proj[13];
                    w = info->proj[15];
                }
                a[i][0] = 0.5 * x + (0.5 + su) * w;
                a[i][1] = -0.5 * y + (0.5 + sv) * w;
                a[i][2] = w;
            }
            // A^-1 (3x3, row-vector convention: s = H A).
            const double (&m)[4][3] = a;
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            if (fabs(det) < 1e-12)
                m_ssrPrevOn = false;
            else
            {
                double inv[3][3];
                inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
                inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
                inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
                inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
                inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
                inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
                inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
                inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
                inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
                // H over s (rows 0..2) and the constant (row 3): H = s A^-1 - t A^-1.
                double h[4][3] = {};
                for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++)
                        h[i][k] = inv[i][k];
                for (int k = 0; k < 3; k++)
                    h[3][k] = -(m[3][0] * inv[0][k] + m[3][1] * inv[1][k] + m[3][2] * inv[2][k]);
                // The previous view (H + c - c') R', then its projection and jitter.
                double c[3], cp[3];
                CameraPlace(info->view, info->camPos, c);
                CameraPlace(m_ssrPrevView, m_ssrPrevCamPos, cp);
                double q[4][4] = {}; // (s, 1) -> the previous view (x, y, z, 1)
                for (int i = 0; i < 4; i++)
                {
                    for (int j = 0; j < 3; j++)
                    {
                        double v = 0;
                        for (int k = 0; k < 3; k++)
                            v += (h[i][k] + (i == 3 ? c[k] - cp[k] : 0.0)) * m_ssrPrevView[k * 4 + j];
                        q[i][j] = v;
                    }
                    q[i][3] = i == 3 ? 1.0 : 0.0;
                }
                const double pu = m_ssrPrevShift[0], pv = m_ssrPrevShift[1];
                for (int i = 0; i < 4; i++)
                {
                    double x = 0, y = 0, w = 0;
                    for (int j = 0; j < 4; j++)
                    {
                        x += q[i][j] * m_ssrPrevProj[j * 4 + 0];
                        y += q[i][j] * m_ssrPrevProj[j * 4 + 1];
                        w += q[i][j] * m_ssrPrevProj[j * 4 + 3];
                    }
                    out[i] = static_cast<float>(0.5 * x + (0.5 + pu) * w);
                    out[4 + i] = static_cast<float>(-0.5 * y + (0.5 + pv) * w);
                    out[8 + i] = static_cast<float>(w);
                }
                out[12] = 1.0f;
                static const bool partDebug = [] {
                    wchar_t v[8] = {};
                    return GetEnvironmentVariableW(L"CRYMT_SSR_PARTICLES", v, 8) && wcscmp(v, L"2") == 0;
                }();
                out[13] = LastParticleLayer() ? (partDebug ? 2.0f : 1.0f) : 0.0f;
                out[14] = static_cast<float>(m_giFullW);
                out[15] = static_cast<float>(m_giFullH);
            }
        }
    }

    void UpdateFrameTexture(const DlssSceneInfo* info)
    {
        if (!m_jitterTex)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = 16;
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&m_jitterTex))))
            {
                m_jitterTex = nullptr;
                return;
            }
            m_jitterCpu = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_jitterTex, nullptr, m_jitterCpu);
        }
        const GiSettings& s = Gi();
        float data[64] = {};
        data[0] = info->shiftU;
        data[1] = info->shiftV;
        data[2] = 0.0f; // the contact shadows' noise frame: fixed (a noise that moved per frame shimmered on a still view)
        memcpy(&data[4], info->sun, 3 * sizeof(float));
        // 2: the old contact shadows; 3: Bend's screen-space shadows (RunSss; the shader reads them in place of the sss
        // placeholder), unless their pipeline failed.
        data[7] = s.contactShadows && info->sun[3] > 0 ? (s.sss && !m_sssFailed ? 3.0f : 2.0f) : 0.0f;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
            {
                float v = 0;
                for (int k = 0; k < 4; k++)
                    v += info->view[r * 4 + k] * info->proj[k * 4 + c];
                data[8 + c * 4 + r] = v;
            }
        data[24] = s.contactLength;
        data[25] = 0.5f;
        data[26] = 50.0f;
        data[27] = s.contactFoliage ? 1.0f : 0.0f;
        // Texel 7: the cube's brightness on the water; which cube (0 the voxel grid's, 1 the engine's RGBK one, 2 its HDR copy,
        // 3 + L that copy read at mip level L: the debug); the near plane; 2 when the water's SSR is on.
        const EnvView waterCube = WaterEnvCube();
        const bool engineCube = waterCube.kind != 0;
        if (m_engineCubeDumpWanted) // the capture key: the world direction to the sun, to place it in the cube's dump
            Log("capture: sun direction %.3f %.3f %.3f (found %.0f)", info->sun[0], info->sun[1], info->sun[2], info->sun[3]);
        data[28] = engineCube ? EngineCubeScale() : WaterSky();
        data[29] = waterCube.kind == 2 ? (g_giControl.envCubeMip >= 0 ? 3.0f + static_cast<float>(g_giControl.envCubeMip) : 2.0f)
                                       : static_cast<float>(waterCube.kind);
        data[30] = info->nearZ;
        data[31] = WaterSsrOn() && !m_hizFailed ? 2.0f : 0.0f;
        WaterPrevFrame(info, data + 32);
        m_hizNearOverFar = info->farZ > 0 ? info->nearZ / info->farZ : 0.0f;
        // The sun for RunSss: its clip position through the unjittered view-projection (a direction: w 0), moved by the
        // frame's jitter as the depth was drawn (the image shifted +x right, +y down: x + 2 shiftU w, y - 2 shiftV w).
        m_sssSun = info->sun[3] > 0;
        m_sssFar = info->farZ;
        for (int c = 0; c < 4; c++)
            m_sssClip[c] = info->sun[0] * data[8 + c * 4] + info->sun[1] * data[9 + c * 4] + info->sun[2] * data[10 + c * 4];
        m_sssClip[0] += 2 * info->shiftU * m_sssClip[3];
        m_sssClip[1] -= 2 * info->shiftV * m_sssClip[3];
        if (memcmp(data, m_frameLast, sizeof(data)) == 0)
            return;
        const UploadAlloc up = FrameUpload(256, 512);
        if (!up.cpu)
            return;
        memcpy(up.cpu, data, sizeof(data));
        m_barriers.push_back(Transition(m_jitterTex, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
        FlushBarriers();
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m_jitterTex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = up.offset;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 16, 1, 1, 256};
        NoteCopy("jitter", dst, 0, 0, 0, src, nullptr);
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m_barriers.push_back(Transition(m_jitterTex, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
        memcpy(m_frameLast, data, sizeof(data));
    }

    // The Forward+ test (CRYMT_FPLUS=1 or the engine page's Num1): -1 until the panel sets it.
    static bool FPlusOn()
    {
        static const bool env = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_FPLUS", v, 8) && v[0] == L'1';
        }();
        const int panel = g_giControl.fplus;
        return panel < 0 ? env : panel > 0;
    }

    // The Forward+ test's texture (the patched fragLib's single-pass light loop reads it, Settings.cpp kFPlusDecl): the
    // frame's point lamps without shadow maps (DlssSceneInfo::fplusLamps) binned into 32-pixel screen tiles by the
    // screen rectangle of their sphere's box (the whole screen when the camera is near it or a corner is behind the
    // camera). Texel 0 (on, tiles across, down, lamps); 1 + 2k, 2 + 2k lamp k's position from the camera and 1 / radius,
    // its colour; 65 the camera; from 128 one per tile (its first index texel, its lamp count); then the indices, 4 a
    // texel. Uploaded when it changed (the rows in use).
    void UpdateFPlus(const DlssSceneInfo* info)
    {
        constexpr UINT kTexW = 1024, kTexH = 128, kTile = 32, kGrid = 128;
        if (!m_fplusTex)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = kTexW;
            d.Height = kTexH;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kSrvState, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&m_fplusTex))))
            {
                m_fplusTex = nullptr;
                return;
            }
            m_fplusCpu = g_srvCpu.Alloc();
            g.dev->CreateShaderResourceView(m_fplusTex, nullptr, m_fplusCpu);
            Log("Forward+ test texture created (%s)", FPlusOn() ? "on" : "off until Num1 on the engine page");
        }
        const UINT sw = info->fplusWidth, sh = info->fplusHeight;
        const UINT tx = (sw + kTile - 1) / kTile, ty = (sh + kTile - 1) / kTile, tiles = tx * ty;
        const UINT count = std::min<UINT>(info->fplusCount, 32);
        const bool on = FPlusOn() && tiles > 0 && tiles <= 16384;
        m_fplusStats.lamps = count;
        m_fplusStats.onScreen = m_fplusStats.litTiles = m_fplusStats.maxPerTile = m_fplusStats.indices = 0;
        if (!on)
        {
            m_fplusData.assign(kTexW * 4, 0.0f);
        }
        else
        {
            float vp[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    float v = 0;
                    for (int k = 0; k < 4; k++)
                        v += info->view[r * 4 + k] * info->proj[k * 4 + c];
                    vp[r * 4 + c] = v;
                }
            const float* cam = info->fplusCam;
            const float nearZ = std::max(info->nearZ, 0.05f);
            struct Rect
            {
                int x0, y0, x1, y1;
            } rects[32];
            m_fplusCount.assign(tiles, 0);
            for (UINT i = 0; i < count; i++)
            {
                const float* l = info->fplusLamps[i];
                Rect& q = rects[i];
                q = {0, 0, -1, -1};
                const float r = l[3] > 0 ? 1.0f / l[3] : 0.0f;
                if (!(r > 0))
                    continue;
                const float d[3] = {l[0] - cam[0], l[1] - cam[1], l[2] - cam[2]};
                const float reach = r + 2 * nearZ + 0.5f;
                bool full = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < reach * reach;
                float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
                int behind = 0;
                for (int k = 0; k < 8 && !full; k++)
                {
                    const float p[3] = {l[0] + (k & 1 ? r : -r), l[1] + (k & 2 ? r : -r), l[2] + (k & 4 ? r : -r)};
                    float clip[4];
                    for (int c = 0; c < 4; c++)
                        clip[c] = p[0] * vp[c] + p[1] * vp[4 + c] + p[2] * vp[8 + c] + vp[12 + c];
                    if (clip[3] < nearZ * 0.5f)
                    {
                        behind++;
                        continue;
                    }
                    const float u = clip[0] / clip[3] * 0.5f + 0.5f, v = -clip[1] / clip[3] * 0.5f + 0.5f;
                    u0 = std::min(u0, u);
                    u1 = std::max(u1, u);
                    v0 = std::min(v0, v);
                    v1 = std::max(v1, v);
                }
                if (behind == 8)
                    continue; // the whole box behind the camera
                if (full || behind)
                {
                    u0 = v0 = 0.0f;
                    u1 = v1 = 1.0f;
                }
                // A pixel's margin (the jitter, the screen coordinate's rounding).
                u0 -= 1.0f / sw;
                v0 -= 1.0f / sh;
                u1 += 1.0f / sw;
                v1 += 1.0f / sh;
                if (u1 < 0 || v1 < 0 || u0 > 1 || v0 > 1)
                    continue;
                const auto cell = [](float f, UINT n) { return std::min(std::max(static_cast<int>(floorf(f * n)), 0), static_cast<int>(n) - 1); };
                q = {cell(u0, tx), cell(v0, ty), cell(u1, tx), cell(v1, ty)};
                m_fplusStats.onScreen++;
            }
            // At most 4 lamps a tile (the shader's slots: its loop count must not vary per pixel), the strongest at the
            // camera first; a lamp left out of a tile keeps the engine's own copy there.
            UINT order[32];
            float weight[32];
            for (UINT i = 0; i < count; i++)
            {
                const float* l = info->fplusLamps[i];
                const float d[3] = {l[0] - cam[0], l[1] - cam[1], l[2] - cam[2]};
                const float r = l[3] > 0 ? 1.0f / l[3] : 0.0f;
                weight[i] = (l[4] + l[5] + l[6]) * r * r / (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + 1.0f);
                order[i] = i;
            }
            std::sort(order, order + count, [&](UINT a, UINT b) { return weight[a] > weight[b]; });
            for (UINT k = 0; k < count; k++)
            {
                const Rect& q = rects[order[k]];
                for (int y = q.y0; y <= q.y1; y++)
                    for (int x = q.x0; x <= q.x1; x++)
                        if (m_fplusCount[y * tx + x] < 4)
                            m_fplusCount[y * tx + x]++;
            }
            m_fplusStart.resize(tiles);
            UINT next = kGrid + tiles;
            for (UINT t = 0; t < tiles; t++)
            {
                m_fplusStart[t] = next;
                next += (m_fplusCount[t] + 3) / 4;
                if (m_fplusCount[t])
                {
                    m_fplusStats.litTiles++;
                    m_fplusStats.maxPerTile = std::max(m_fplusStats.maxPerTile, m_fplusCount[t]);
                    m_fplusStats.indices += m_fplusCount[t];
                }
            }
            if (next > kTexW * kTexH)
                return; // cannot happen: 16384 tiles + 16384 * 8 index texels fit
            const UINT rows = (next + kTexW - 1) / kTexW;
            m_fplusData.assign(static_cast<size_t>(rows) * kTexW * 4, 0.0f);
            float* o = m_fplusData.data();
            o[0] = 1.0f;
            o[1] = static_cast<float>(tx);
            o[2] = static_cast<float>(ty);
            o[3] = static_cast<float>(count);
            for (UINT i = 0; i < count; i++)
            {
                const float* l = info->fplusLamps[i];
                float* p = o + (1 + 2 * i) * 4;
                p[0] = l[0] - cam[0];
                p[1] = l[1] - cam[1];
                p[2] = l[2] - cam[2];
                p[3] = l[3];
                memcpy(p + 4, l + 4, 4 * sizeof(float));
            }
            memcpy(o + 65 * 4, cam, 4 * sizeof(float));
            for (UINT t = 0; t < tiles; t++)
            {
                o[(kGrid + t) * 4 + 0] = static_cast<float>(m_fplusStart[t]);
                o[(kGrid + t) * 4 + 1] = static_cast<float>(m_fplusCount[t]);
                m_fplusCount[t] = 0; // now the fill position
            }
            for (UINT k = 0; k < count; k++)
            {
                const Rect& q = rects[order[k]];
                for (int y = q.y0; y <= q.y1; y++)
                    for (int x = q.x0; x <= q.x1; x++)
                    {
                        const UINT t = y * tx + x;
                        if (m_fplusCount[t] < static_cast<UINT>(o[(kGrid + t) * 4 + 1]))
                            o[m_fplusStart[t] * 4 + m_fplusCount[t]++] = static_cast<float>(order[k]);
                    }
            }
        }
        if (m_fplusData == m_fplusLast)
            return;
        const UINT rows = static_cast<UINT>(m_fplusData.size() / (kTexW * 4));
        const uint64_t bytes = static_cast<uint64_t>(rows) * kTexW * 16;
        const UploadAlloc up = FrameUpload(bytes, 512);
        if (!up.cpu)
            return;
        memcpy(up.cpu, m_fplusData.data(), bytes);
        m_barriers.push_back(Transition(m_fplusTex, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
        FlushBarriers();
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m_fplusTex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = up.offset;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kTexW, rows, 1, kTexW * 16};
        NoteCopy("fplus", dst, 0, 0, 0, src, nullptr);
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m_barriers.push_back(Transition(m_fplusTex, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
        m_fplusLast = m_fplusData;
        if (m_fplusStats.uploads++ % 1000 == 0)
            Log("Forward+ test: %s, %u lamps, %u on screen, %u of %u tiles lit, max %u a tile, %u indices, %u rows", on ? "on" : "off",
                m_fplusStats.lamps, m_fplusStats.onScreen, m_fplusStats.litTiles, tiles, m_fplusStats.maxPerTile, m_fplusStats.indices, rows);
    }

    void GiFrameCamera(const DlssSceneInfo* info)
    {
        m_pastScene = false;
        if (info->valid && g_jitterPlaceholder)
            UpdateFrameTexture(info);
        if (info->valid && g_fplusPlaceholder)
            UpdateFPlus(info);
        g_wantPassMatrices.store(Gi().on && ((Gi().geometry && (Gi().shadowGeometry || Gi().cubeGeometry || Gi().cubeInject)) ||
                                             (Gi().vfog && Gi().vfogSun > 0)),
                                 std::memory_order_relaxed);
        if (info->reset) // the sun's maps of another place or level (the fog's shadows): their textures may be gone
            for (SunMap& s : m_sunMaps)
                s = SunMap{};
        GiCamera(info);
        m_giInScene = info->valid != 0;
        // A frame whose GI pass did not run leaves its draws: dropped with the first camera of the next frame. Not at
        // every camera: the engine sets the main camera again within a frame (before the z-prepass, the general pass,
        // after the water), and that dropped the z-prepass draws recorded before it - CryMT's own z-prepass draws the
        // solid geometry first - so the GI's voxels got the alpha-tested ones (foliage) and almost no ground, walls or
        // rocks (the user: holes across roads, steps; a capture: 1003 solid draws recorded a frame, 17 voxelized).
        const uint64_t frame = g_presented.load(std::memory_order_relaxed);
        if (frame != m_voxDrawsFrame)
        {
            m_voxDraws.clear();
            m_voxCameraCopies.clear();
            m_voxDrawsFrame = frame;
        }
    }

    // The main camera of the frame being drawn (stream order: before its draws), for the GI pass.
    void GiCamera(const DlssSceneInfo* info)
    {
        if (info->valid)
        {
            memcpy(m_giView, info->view, sizeof(m_giView));
            memcpy(m_giProj, info->proj, sizeof(m_giProj));
            memcpy(m_giCamPos, info->camPos, sizeof(m_giCamPos));
            m_giHaveCamera = true;
        }
    }

    // Full-size pixels per GI texel each way: 2 up to about 0.65 million GI texels (1920x1080 gives 0.52), else 4
    // (5120x1440: 0.46 instead of 1.84; the trace cost 3 ms of GPU there at half size). CRYMT_GI_SCALE=2 or 4 forces it.
    static UINT GiScale(UINT width, UINT height)
    {
        static const int forced = [] {
            wchar_t text[8] = {};
            return GetEnvironmentVariableW(L"CRYMT_GI_SCALE", text, 8) ? _wtoi(text) : 0;
        }();
        if (forced == 2 || forced == 4)
            return static_cast<UINT>(forced);
        return (width / 2) * (height / 2) <= 650000 ? 2 : 4;
    }
