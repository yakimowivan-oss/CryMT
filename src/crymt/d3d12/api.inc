// CryMT D3D12 back end, class Device: the ID3D10Device1 interface: state, draws, copies, getters, creation.
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // In the engine's cube render (m_engineCubePass): its encode pass binds the face's scene (TempEnvCMap) as its input
    // while it is still the render target (the cube's face is set after, applied at the draw), which D3D10 turns into
    // no input (the cube came out black). There the target is let go instead: the engine replaces it before the draw.
    bool ReleaseOutput(Srv12* v)
    {
        for (Rtv12*& rtv : m_rtv)
            if (rtv && rtv->resource == v->resource)
            {
                rtv->info->outputBound--;
                Bind(rtv, static_cast<Rtv12*>(nullptr));
                m_omDirty = m_psoDirty = true;
                m_stats.engineCubeUnbinds++;
            }
        return v->info->outputBound == 0;
    }

    void SetShaderResources(Stage s, UINT start, UINT n, ID3D10ShaderResourceView* const* views)
    {
        StageState& st = m_stage[s];
        for (UINT i = 0; i < n && start + i < D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++)
        {
            auto* v = views ? static_cast<Srv12*>(static_cast<ID3D10ShaderResourceView1*>(views[i])) : nullptr;
            if (v && v->info->outputBound && !(m_engineCubePass && ReleaseOutput(v))) // D3D10: a resource bound for output cannot be an input
                v = nullptr;

            if (st.srv[start + i] != v)
            {
                Bind(st.srv[start + i], v);
                st.srvDirty = true;
            }
        }
        if (n)
            st.srvEnd = std::max(st.srvEnd, std::min(start + n, static_cast<UINT>(D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)));
    }

    void SetSamplers(Stage s, UINT start, UINT n, ID3D10SamplerState* const* samplers)
    {
        StageState& st = m_stage[s];
        for (UINT i = 0; i < n && start + i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++)
        {
            auto* smp = samplers ? static_cast<Sampler12*>(samplers[i]) : nullptr;
            if (st.sampler[start + i] != smp)
            {
                Bind(st.sampler[start + i], smp);
                st.samplerDirty = true;
            }
        }
    }

    template <class T>
    void GetArray(T* const* from, UINT start, UINT n, T** to, UINT limit)
    {
        for (UINT i = 0; i < n; i++)
        {
            T* v = start + i < limit ? from[start + i] : nullptr;
            if (v)
                v->AddRef();
            to[i] = v;
        }
    }

    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT start, UINT n, ID3D10Buffer* const* b) override
    {
        SetConstantBuffers(kVS, start, n, b);
    }
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT start, UINT n, ID3D10Buffer* const* b) override
    {
        SetConstantBuffers(kGS, start, n, b);
    }
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT start, UINT n, ID3D10Buffer* const* b) override
    {
        SetConstantBuffers(kPS, start, n, b);
    }
    void STDMETHODCALLTYPE VSSetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView* const* v) override
    {
        SetShaderResources(kVS, start, n, v);
    }
    void STDMETHODCALLTYPE GSSetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView* const* v) override
    {
        SetShaderResources(kGS, start, n, v);
    }
    void STDMETHODCALLTYPE PSSetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView* const* v) override
    {
        SetShaderResources(kPS, start, n, v);
    }
    void STDMETHODCALLTYPE VSSetSamplers(UINT start, UINT n, ID3D10SamplerState* const* s) override
    {
        SetSamplers(kVS, start, n, s);
    }
    void STDMETHODCALLTYPE GSSetSamplers(UINT start, UINT n, ID3D10SamplerState* const* s) override
    {
        SetSamplers(kGS, start, n, s);
    }
    void STDMETHODCALLTYPE PSSetSamplers(UINT start, UINT n, ID3D10SamplerState* const* s) override
    {
        SetSamplers(kPS, start, n, s);
    }

    void STDMETHODCALLTYPE VSSetShader(ID3D10VertexShader* s) override
    {
        auto* v = static_cast<VS12*>(s);
        if (v != m_vs)
        {
            Bind(m_vs, v);
            m_psoDirty = true;
        }
    }
    void STDMETHODCALLTYPE GSSetShader(ID3D10GeometryShader* s) override
    {
        auto* v = static_cast<GS12*>(s);
        if (v != m_gs)
        {
            Bind(m_gs, v);
            m_psoDirty = true;
        }
    }
    void STDMETHODCALLTYPE PSSetShader(ID3D10PixelShader* s) override
    {
        auto* v = static_cast<PS12*>(s);
        if (v != m_ps)
        {
            const auto env = [](const PS12* p) { return p && (p->envCubeSlot >= 0 || p->env2DSlot >= 0); };
            if (env(v) || env(m_ps))
                m_stage[kPS].srvDirty = true;
            Bind(m_ps, v);
            m_psoDirty = true;
        }
    }

    // ---- input assembler
    void STDMETHODCALLTYPE IASetInputLayout(ID3D10InputLayout* layout) override
    {
        auto* l = static_cast<Layout12*>(layout);
        if (l != m_layout)
        {
            Bind(m_layout, l);
            m_psoDirty = true;
        }
    }

    void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT n, ID3D10Buffer* const* buffers, const UINT* strides, const UINT* offsets) override
    {
        for (UINT i = 0; i < n && start + i < 16; i++)
        {
            VertexBinding& vb = m_vb[start + i];
            Bind(vb.buffer, buffers ? static_cast<Buffer12*>(buffers[i]) : nullptr);
            vb.stride = strides ? strides[i] : 0;
            vb.offset = offsets ? offsets[i] : 0;
        }
    }

    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D10Buffer* buffer, DXGI_FORMAT format, UINT offset) override
    {
        Bind(m_ib, static_cast<Buffer12*>(buffer));
        if (format != m_ibFormat && StripTopology(m_topology))
            m_psoDirty = true;
        m_ibFormat = format;
        m_ibOffset = offset;
    }

    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY t) override
    {
        if (t == m_topology)
            return;
        if (TopologyType(t) != TopologyType(m_topology) || StripTopology(t) != StripTopology(m_topology))
            m_psoDirty = true;
        m_topology = t;
        m_topologyDirty = true;
    }

    // ---- output merger
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, ID3D10RenderTargetView* const* views, ID3D10DepthStencilView* depth) override
    {
        Rtv12* rtv[8] = {};
        for (UINT i = 0; i < n && i < 8 && views; i++)
            rtv[i] = static_cast<Rtv12*>(views[i]);
        auto* dsv = static_cast<Dsv12*>(depth);
        for (UINT i = 0; i < 8; i++)
        {
            if (rtv[i])
                rtv[i]->info->outputBound++;
            if (m_rtv[i])
                m_rtv[i]->info->outputBound--;
        }
        if (dsv)
            dsv->info->outputBound++;
        if (m_dsv)
            m_dsv->info->outputBound--;
        for (UINT i = 0; i < 8; i++)
        {
            Bind(m_rtv[i], rtv[i]);
            if (rtv[i])
                UnbindInputs(rtv[i]->resource);
        }
        Bind(m_dsv, dsv);
        if (dsv)
            UnbindInputs(dsv->resource);
        m_omDirty = true;
        m_psoDirty = true;
    }

    // D3D10's hazard rule: a resource bound for output is unbound from every shader input slot.
    void UnbindInputs(ID3D10Resource* resource)
    {
        for (StageState& st : m_stage)
            for (UINT i = 0; i < st.srvEnd; i++)
                if (st.srv[i] && st.srv[i]->resource == resource)
                {
                    Bind(st.srv[i], static_cast<Srv12*>(nullptr));
                    st.srvDirty = true;
                }
    }

    void STDMETHODCALLTYPE OMSetBlendState(ID3D10BlendState* state, const FLOAT factor[4], UINT mask) override
    {
        auto* b = static_cast<Blend12*>(static_cast<ID3D10BlendState1*>(state));
        if (b != m_blend || mask != m_sampleMask)
            m_psoDirty = true;
        Bind(m_blend, b);
        m_sampleMask = mask;
        const float one[4] = {1, 1, 1, 1};
        const float* f = factor ? factor : one;
        if (memcmp(f, m_blendFactor, sizeof(m_blendFactor)) != 0)
        {
            memcpy(m_blendFactor, f, sizeof(m_blendFactor));
            m_blendFactorDirty = true;
        }
    }

    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D10DepthStencilState* state, UINT ref) override
    {
        auto* d = static_cast<Depth12*>(state);
        if (d != m_depth)
        {
            Bind(m_depth, d);
            m_psoDirty = true;
        }
        if (ref != m_stencilRef)
        {
            m_stencilRef = ref;
            m_stencilRefDirty = true;
        }
    }

    void STDMETHODCALLTYPE SOSetTargets(UINT n, ID3D10Buffer* const* buffers, const UINT*) override
    {
        for (UINT i = 0; i < n; i++)
            if (buffers && buffers[i])
                LogLimited("so", "SOSetTargets with a buffer is not supported");
    }

    void STDMETHODCALLTYPE SetPredication(ID3D10Predicate* p, WINBOOL) override
    {
        if (p)
            LogLimited("pred", "SetPredication is not supported");
    }

    // ---- rasterizer
    void STDMETHODCALLTYPE RSSetState(ID3D10RasterizerState* state) override
    {
        auto* r = static_cast<Raster12*>(state);
        if (r == m_raster)
            return;
        Bind(m_raster, r);
        m_psoDirty = true;
        m_scissorDirty = true;
    }

    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D10_VIEWPORT* vps) override
    {
        m_numViewports = std::min(n, 16u);
        for (UINT i = 0; i < m_numViewports; i++)
        {
            m_vp10[i] = vps[i];
            m_viewports[i] = {static_cast<float>(vps[i].TopLeftX), static_cast<float>(vps[i].TopLeftY), static_cast<float>(vps[i].Width),
                              static_cast<float>(vps[i].Height), vps[i].MinDepth, vps[i].MaxDepth};
        }
        m_viewportDirty = true;
        m_scissorDirty = true;
    }

    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D10_RECT* rects) override
    {
        m_numScissors = std::min(n, 16u);
        for (UINT i = 0; i < m_numScissors; i++)
            m_scissors[i] = rects[i];
        m_scissorDirty = true;
    }

    // ---- draws
    // Where the ocean's draws come (for leaving it out of the voxels): logged for one frame in 600.
    void NoteWaterDraw()
    {
        if (!m_vs || !m_vs->water || (m_giFrame % 600) != 7)
            return;
        Log("ocean draw at GI frame %llu: GI ran %d, rt0 format %d, scene depth %d, depth write %d, z-prepass recording %d",
            static_cast<unsigned long long>(m_giFrame), m_giLastRun == m_giFrame ? 1 : 0, m_rtv[0] ? static_cast<int>(m_rtv[0]->format) : -1,
            m_dsv && m_dsv->info == m_giSceneDepth ? 1 : 0, m_depth ? static_cast<int>(m_depth->d12.DepthWriteMask) : -1, VoxRecording() ? 1 : 0);
    }

    void STDMETHODCALLTYPE DrawIndexed(UINT count, UINT start, INT base) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(true))
        {
            m_list->DrawIndexedInstanced(count, 1, start, base, 0); m_stats.vertices += count;
            if (VoxRecording())
                RecordVoxDraw(false, true, count, 1, start, base, 0);
            else if (ShadowRecording())
                RecordVoxDraw(true, true, count, 1, start, base, 0);
            else if (CubeRecording())
                RecordVoxDraw(2, true, count, 1, start, base, 0);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE Draw(UINT count, UINT start) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(false))
        {
            m_list->DrawInstanced(count, 1, start, 0); m_stats.vertices += count;
            if (VoxRecording())
                RecordVoxDraw(false, false, count, 1, start, 0, 0);
            else if (ShadowRecording())
                RecordVoxDraw(true, false, count, 1, start, 0, 0);
            else if (CubeRecording())
                RecordVoxDraw(2, false, count, 1, start, 0, 0);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT count, UINT instances, UINT start, INT base, UINT startInstance) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(true))
        {
            m_list->DrawIndexedInstanced(count, instances, start, base, startInstance); m_stats.vertices += count * instances;
            if (VoxRecording())
                RecordVoxDraw(false, true, count, instances, start, base, startInstance);
            else if (ShadowRecording())
                RecordVoxDraw(true, true, count, instances, start, base, startInstance);
            else if (CubeRecording())
                RecordVoxDraw(2, true, count, instances, start, base, startInstance);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE DrawInstanced(UINT count, UINT instances, UINT start, UINT startInstance) override
    {
        NoteWaterDraw();
        KeepObjectMotion();
        if (Apply(false))
        {
            m_list->DrawInstanced(count, instances, start, startInstance); m_stats.vertices += count * instances;
            if (VoxRecording())
                RecordVoxDraw(false, false, count, instances, start, 0, startInstance);
            else if (ShadowRecording())
                RecordVoxDraw(true, false, count, instances, start, 0, startInstance);
            else if (CubeRecording())
                RecordVoxDraw(2, false, count, instances, start, 0, startInstance);
        }
        AfterDraw();
    }

    void STDMETHODCALLTYPE DrawAuto() override
    {
        LogLimited("drawauto", "DrawAuto is not supported");
    }

    // Clear census: which targets are cleared to what, to give them matching optimized clear values (fast clears).
    std::map<std::string, uint64_t> m_clears;

    void NoteClear(const TextureCore* t, const FLOAT* color, float depth, UINT flagsAndStencil)
    {
        char key[160];
        if (color)
            snprintf(key, sizeof(key), "RT %ux%u fmt %d color %.3g %.3g %.3g %.3g", t->width, t->height, t->format, color[0], color[1], color[2], color[3]);
        else
            snprintf(key, sizeof(key), "DS %ux%u fmt %d flags %u depth %.3g stencil %u", t->width, t->height, t->format, flagsAndStencil & 0xFF, depth,
                     flagsAndStencil >> 8);
        m_clears[key]++;
    }

    // Barrier census: transitions by resource and states.
    std::map<std::string, uint64_t> m_barrierKinds;

    void NoteBarrier(const ResourceInfo* info, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        char key[160];
        if (info)
        {
            const auto* t = static_cast<const TextureCore*>(info);
            snprintf(key, sizeof(key), "tex %ux%u fmt %d bind 0x%x: 0x%x -> 0x%x", t->width, t->height, t->format, t->bind, before, after);
        }
        else
            snprintf(key, sizeof(key), "buffer page: 0x%x -> 0x%x", before, after);
        m_barrierKinds[key]++;
    }

    // A draw packet (DrawPacket.h; CryMT's own per-object loops): its constants go to frame upload memory and replace
    // the bound buffer of their slot for this draw only. The slot's binding itself stays: its applied address no longer
    // matches, so the next draw applies the bound buffer again.
    void DrawIndexedWithConstants(const uint8_t* packet)
    {
        DrawPacketHeader h;
        memcpy(&h, packet, sizeof(h));
        m_stats.packets++;
        // The packet's own streams and index buffer, after its constants; the bindings they replace come back after.
        const uint8_t* q = packet + sizeof(h);
        for (uint32_t i = 0; i < h.constants; i++)
        {
            DrawPacketConstants c;
            memcpy(&c, q, sizeof(c));
            q += sizeof(c) + c.vectors * 16u;
        }
        struct Saved
        {
            UINT slot;
            ID3D10Buffer* buffer;
            UINT stride, offset;
        } saved[kDrawPacketMaxStreams];
        UINT savedCount = 0;
        for (uint32_t i = 0; i < h.streams && savedCount < kDrawPacketMaxStreams; i++)
        {
            DrawPacketStream s;
            memcpy(&s, q, sizeof(s));
            q += sizeof(s);
            if (s.slot >= 16)
                continue;
            VertexBinding& vb = m_vb[s.slot];
            saved[savedCount] = {s.slot, vb.buffer, vb.stride, vb.offset};
            if (vb.buffer)
                vb.buffer->AddRef();
            savedCount++;
            ID3D10Buffer* b = reinterpret_cast<ID3D10Buffer*>(s.buffer);
            IASetVertexBuffers(s.slot, 1, &b, &s.stride, &s.offset);
        }
        ID3D10Buffer* savedIb = nullptr;
        DXGI_FORMAT savedIbFormat = m_ibFormat;
        UINT savedIbOffset = m_ibOffset;
        if (h.indexBuffer)
        {
            DrawPacketIndex x;
            memcpy(&x, q, sizeof(x));
            savedIb = m_ib;
            if (savedIb)
                savedIb->AddRef();
            IASetIndexBuffer(reinterpret_cast<ID3D10Buffer*>(x.buffer), static_cast<DXGI_FORMAT>(x.format), x.offset);
        }
        for (int attempt = 0; attempt < 3; attempt++)
        {
            const uint64_t serial = g_mainSerial;
            if (!Apply(true))
                break;
            D3D12_GPU_VIRTUAL_ADDRESS va[kDrawPacketMaxConstants];
            Stage stage[kDrawPacketMaxConstants];
            UINT slot[kDrawPacketMaxConstants];
            UINT n = 0;
            const uint8_t* p = packet + sizeof(h);
            for (uint32_t i = 0; i < h.constants && i < kDrawPacketMaxConstants; i++)
            {
                DrawPacketConstants c;
                memcpy(&c, p, sizeof(c));
                const uint32_t bytes = c.vectors * 16u;
                const Stage s = c.stage == 0 ? kVS : c.stage == 1 ? kPS : kGS;
                if (bytes && c.slot < kRootCbSlots[s])
                {
                    const UploadAlloc up = FrameUpload((bytes + 255) & ~uint64_t{255}, 256);
                    if (up.cpu)
                    {
                        memcpy(up.cpu, p + sizeof(c), bytes);
                        va[n] = up.res->GetGPUVirtualAddress() + up.offset;
                        stage[n] = s;
                        slot[n] = c.slot;
                        n++;
                    }
                }
                p += sizeof(c) + bytes;
            }
            if (serial != g_mainSerial)
                continue; // an upload submitted the list: apply again in the new one
            for (UINT i = 0; i < n; i++)
            {
                m_list->SetGraphicsRootConstantBufferView(kRootCb[stage[i]] + slot[i], va[i]);
                m_stage[stage[i]].cbApplied[slot[i]] = va[i];
            }
            m_list->DrawIndexedInstanced(h.count, 1, h.start, h.base, 0);
            if (VoxRecording())
                RecordVoxDraw(false, true, h.count, 1, h.start, h.base, 0);
            else if (ShadowRecording())
                RecordVoxDraw(true, true, h.count, 1, h.start, h.base, 0);
            else if (CubeRecording())
                RecordVoxDraw(2, true, h.count, 1, h.start, h.base, 0);
            break;
        }
        for (UINT i = 0; i < savedCount; i++)
        {
            IASetVertexBuffers(saved[i].slot, 1, &saved[i].buffer, &saved[i].stride, &saved[i].offset);
            if (saved[i].buffer)
                saved[i].buffer->Release();
        }
        if (h.indexBuffer)
        {
            IASetIndexBuffer(savedIb, savedIbFormat, savedIbOffset);
            if (savedIb)
                savedIb->Release();
        }
        AfterDraw();
    }

    // The engine's object motion blur target is reused soon after its pass: at the overview spot (19 motion blur draws a
    // frame into their own RGBA16F target) the target held a copy of the scene colour by the time the scene was handed
    // to DLSS, so RunMotion read colours as velocities (in 44% of the frames; its distance check threw most away, which
    // also lost the real velocities - the "vegetation garbage" of 2026-11-05). The first draw, clear or copy after the
    // pass (or the hand-over) copies the target into m_ombCopy, which RunMotion and the CRYMT_MVSEQ dumps read.
    void KeepObjectMotion()
    {
        if (!m_ombPending || (m_vs && m_vs->motionBlur))
            return;
        m_ombPending = false;
        TextureCore* src = m_ombTarget;
        if (!src || !src->res || src->format != DXGI_FORMAT_R16G16B16A16_FLOAT || src->samples != 1 || src->dim != D3D10_RESOURCE_DIMENSION_TEXTURE2D)
            return;
        if (!m_ombCopy.res || m_ombCopy.width != src->width || m_ombCopy.height != src->height)
        {
            DeferRelease(m_ombCopy.res);
            m_ombCopy.res = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = src->width;
            d.Height = src->height;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&m_ombCopy.res))))
            {
                m_ombCopy.res = nullptr;
                m_ombCopy.width = m_ombCopy.height = 0;
                return;
            }
            m_ombCopy.dim = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            m_ombCopy.width = src->width;
            m_ombCopy.height = src->height;
            m_ombCopy.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_ombCopy.samples = 1;
            m_ombCopy.state = D3D12_RESOURCE_STATE_COPY_DEST;
            Log("object motion: the motion blur target kept in a copy, %ux%u", src->width, src->height);
        }
        const D3D12_RESOURCE_STATES before = src->state;
        Require(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Require(&m_ombCopy, D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        m_list->CopyResource(m_ombCopy.res, src->res);
        Require(src, before);
        FlushBarriers();
        m_ombCopyFrame = m_frame;
        m_stats.objectMotionCopies++;
    }

    void AfterDraw()
    {
        if (m_vs && m_vs->motionBlur && m_rtv[0])
        {
            m_ombTarget = static_cast<TextureCore*>(m_rtv[0]->info);
            m_ombFrame = m_frame;
            m_ombPending = true;
            // Diagnosis (2026-11-13: at the overview spot the "motion blur target" held the scene colour): the draws with
            // MotionBlurData in the vertex shader this frame, by target and shaders.
            if (m_ombDiagFrame != m_frame)
            {
                m_ombDiagFrame = m_frame;
                m_ombDiagCount = 0;
            }
            if (m_ombDiagCount < 8)
                m_ombDiag[m_ombDiagCount++] = {m_ombTarget, m_vs->contentHash, m_ps ? m_ps->contentHash : 0};
            else
                m_ombDiagCount++;
        }
        m_stats.draws++;
        if (m_engineCubeRes && m_rtv[0] && m_rtv[0]->resource == m_engineCubeRes)
        {
            m_stats.engineCubeDraws++;
            const D3D10_RENDER_TARGET_VIEW_DESC& d = m_rtv[0]->desc;
            const UINT face = d.ViewDimension == D3D10_RTV_DIMENSION_TEXTURE2DARRAY ? d.Texture2DArray.FirstArraySlice : 0;
            m_stats.engineCubeFaces[face < 6 ? face : 5]++;
            m_engineCubeLastFace = static_cast<int>(face < 6 ? face : 5);
            if (m_engineCubeDumpWanted)
            {
                // The capture key: what the next draw into the cube (the engine's Encode_ToLDR) read, as <tag>_envsrc.
                m_engineCubeDumpWanted = false;
                for (int slot = 0; slot < 4; slot++)
                    if (Srv12* s = m_stage[kPS].srv[slot]; s && s->info && !s->info->buffer)
                    {
                        const D3D12_RESOURCE_DESC rd = s->info->res->GetDesc();
                        Log("engine cube draw (face %u): PS slot %d %llux%u format %d array %u", face, slot,
                            static_cast<unsigned long long>(rd.Width), rd.Height, static_cast<int>(rd.Format), rd.DepthOrArraySize);
                        if (slot == 0)
                        {
                            Require(s->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
                            FlushBarriers();
                            RecordDump(s->info->res, "envsrc", 0);
                            Require(s->info, kSrvState);
                            FlushBarriers();
                        }
                    }
            }
        }
        if (++m_drawsSinceSubmit >= 2048 && !m_activeQueries)
            Submit();
    }

    // ---- clears, copies, updates
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D10RenderTargetView* view, const FLOAT color[4]) override
    {
        KeepObjectMotion();
        auto* v = static_cast<Rtv12*>(view);
        if (!v)
            return;
        Require(v->info, D3D12_RESOURCE_STATE_RENDER_TARGET);
        FlushBarriers();
        m_list->ClearRenderTargetView(v->cpu, color, 0, nullptr);
        m_stats.rtClears++;
        NoteClear(static_cast<TextureCore*>(v->info), color, 0.0f, 0);
    }

    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D10DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil) override
    {
        auto* v = static_cast<Dsv12*>(view);
        if (!v)
            return;
        Require(v->info, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        FlushBarriers();
        m_list->ClearDepthStencilView(v->cpu, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil, 0, nullptr);
        m_stats.dsClears++;
        NoteClear(static_cast<TextureCore*>(v->info), nullptr, depth, flags | stencil << 8);
    }

    void STDMETHODCALLTYPE CopyResource(ID3D10Resource* dst, ID3D10Resource* src) override
    {
        KeepObjectMotion();
        if (!dst || !src)
            return;
        Buffer12* db = AsBuffer(dst);
        Buffer12* sb = AsBuffer(src);
        if (db && sb)
        {
            CopyBuffer(db, 0, sb, 0, std::min(db->desc.ByteWidth, sb->desc.ByteWidth));
            return;
        }
        TextureCore* dt = AsTexture(dst);
        TextureCore* st = AsTexture(src);
        if (!m_pastScene && dt && st && LikeSceneColour(st) && LikeSceneColour(dt))
            NotePreCopy(dt); // a copy of the scene colour before DLSS (jittered)
        if (!dt || !st)
            return;
        if (dt->Staging() || st->Staging())
        {
            for (UINT sub = 0; sub < dt->subresources && sub < st->subresources; sub++)
                CopyTexture(dt, sub, 0, 0, 0, st, sub, nullptr);
            return;
        }
        Require(dt, D3D12_RESOURCE_STATE_COPY_DEST);
        Require(st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        FlushBarriers();
        m_list->CopyResource(dt->res, st->res);
    }

    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D10Resource* dst, UINT dstSub, UINT x, UINT y, UINT z, ID3D10Resource* src, UINT srcSub,
                                                 const D3D10_BOX* box) override
    {
        KeepObjectMotion();
        if (!dst || !src)
            return;
        Buffer12* db = AsBuffer(dst);
        Buffer12* sb = AsBuffer(src);
        if (db && sb)
        {
            const UINT from = box ? box->left : 0;
            const UINT bytes = box ? box->right - box->left : sb->desc.ByteWidth;
            CopyBuffer(db, x, sb, from, bytes);
            return;
        }
        TextureCore* dt = AsTexture(dst);
        TextureCore* st = AsTexture(src);
        if (dt && st)
            CopyTexture(dt, dstSub, x, y, z, st, srcSub, box);
    }

    void STDMETHODCALLTYPE UpdateSubresource(ID3D10Resource* dst, UINT sub, const D3D10_BOX* box, const void* data, UINT rowPitch, UINT depthPitch) override;

    void STDMETHODCALLTYPE GenerateMips(ID3D10ShaderResourceView*) override
    {
        LogLimited("mips", "GenerateMips is not implemented");
    }

    void STDMETHODCALLTYPE ResolveSubresource(ID3D10Resource* dst, UINT dstSub, ID3D10Resource* src, UINT srcSub, DXGI_FORMAT format) override
    {
        KeepObjectMotion();
        TextureCore* dt = AsTexture(dst);
        TextureCore* st = AsTexture(src);
        if (!dt || !st)
            return;
        Require(dt, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        Require(st, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        FlushBarriers();
        m_list->ResolveSubresource(dt->res, dstSub, st->res, srcSub, format);
    }

    // ---- getters
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT start, UINT n, ID3D10Buffer** out) override
    {
        GetArray(reinterpret_cast<ID3D10Buffer* const*>(m_stage[kVS].cb), start, n, out, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT start, UINT n, ID3D10Buffer** out) override
    {
        GetArray(reinterpret_cast<ID3D10Buffer* const*>(m_stage[kGS].cb), start, n, out, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT start, UINT n, ID3D10Buffer** out) override
    {
        GetArray(reinterpret_cast<ID3D10Buffer* const*>(m_stage[kPS].cb), start, n, out, D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    }
    void GetViews(Stage s, UINT start, UINT n, ID3D10ShaderResourceView** out)
    {
        for (UINT i = 0; i < n; i++)
        {
            Srv12* v = start + i < D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ? m_stage[s].srv[start + i] : nullptr;
            if (v)
                v->AddRef();
            out[i] = v;
        }
    }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView** out) override
    {
        GetViews(kVS, start, n, out);
    }
    void STDMETHODCALLTYPE GSGetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView** out) override
    {
        GetViews(kGS, start, n, out);
    }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT start, UINT n, ID3D10ShaderResourceView** out) override
    {
        GetViews(kPS, start, n, out);
    }
    void STDMETHODCALLTYPE VSGetSamplers(UINT start, UINT n, ID3D10SamplerState** out) override
    {
        GetArray(reinterpret_cast<ID3D10SamplerState* const*>(m_stage[kVS].sampler), start, n, out, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE GSGetSamplers(UINT start, UINT n, ID3D10SamplerState** out) override
    {
        GetArray(reinterpret_cast<ID3D10SamplerState* const*>(m_stage[kGS].sampler), start, n, out, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT);
    }
    void STDMETHODCALLTYPE PSGetSamplers(UINT start, UINT n, ID3D10SamplerState** out) override
    {
        GetArray(reinterpret_cast<ID3D10SamplerState* const*>(m_stage[kPS].sampler), start, n, out, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT);
    }
    template <class T, class U>
    static void GetOne(T* v, U** out)
    {
        if (v)
            v->AddRef();
        *out = v;
    }
    void STDMETHODCALLTYPE VSGetShader(ID3D10VertexShader** out) override
    {
        GetOne(m_vs, out);
    }
    void STDMETHODCALLTYPE GSGetShader(ID3D10GeometryShader** out) override
    {
        GetOne(m_gs, out);
    }
    void STDMETHODCALLTYPE PSGetShader(ID3D10PixelShader** out) override
    {
        GetOne(m_ps, out);
    }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D10InputLayout** out) override
    {
        GetOne(m_layout, out);
    }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT start, UINT n, ID3D10Buffer** buffers, UINT* strides, UINT* offsets) override
    {
        for (UINT i = 0; i < n; i++)
        {
            const VertexBinding none{};
            const VertexBinding& vb = start + i < 16 ? m_vb[start + i] : none;
            if (buffers)
                GetOne(vb.buffer, &buffers[i]);
            if (strides)
                strides[i] = vb.stride;
            if (offsets)
                offsets[i] = vb.offset;
        }
    }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D10Buffer** buffer, DXGI_FORMAT* format, UINT* offset) override
    {
        if (buffer)
            GetOne(m_ib, buffer);
        if (format)
            *format = m_ibFormat;
        if (offset)
            *offset = m_ibOffset;
    }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY* t) override
    {
        *t = m_topology;
    }
    void STDMETHODCALLTYPE GetPredication(ID3D10Predicate** p, WINBOOL* value) override
    {
        if (p)
            *p = nullptr;
        if (value)
            *value = FALSE;
    }
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT n, ID3D10RenderTargetView** views, ID3D10DepthStencilView** depth) override
    {
        for (UINT i = 0; views && i < n; i++)
            GetOne(i < 8 ? m_rtv[i] : nullptr, &views[i]);
        if (depth)
            GetOne(m_dsv, depth);
    }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D10BlendState** state, FLOAT factor[4], UINT* mask) override
    {
        if (state)
        {
            ID3D10BlendState1* b = m_blend;
            GetOne(b, reinterpret_cast<ID3D10BlendState1**>(state));
        }
        if (factor)
            memcpy(factor, m_blendFactor, sizeof(m_blendFactor));
        if (mask)
            *mask = m_sampleMask;
    }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D10DepthStencilState** state, UINT* ref) override
    {
        if (state)
            GetOne(m_depth, state);
        if (ref)
            *ref = m_stencilRef;
    }
    void STDMETHODCALLTYPE SOGetTargets(UINT n, ID3D10Buffer** buffers, UINT* offsets) override
    {
        for (UINT i = 0; i < n; i++)
        {
            if (buffers)
                buffers[i] = nullptr;
            if (offsets)
                offsets[i] = 0;
        }
    }
    void STDMETHODCALLTYPE RSGetState(ID3D10RasterizerState** state) override
    {
        GetOne(m_raster, state);
    }
    void STDMETHODCALLTYPE RSGetViewports(UINT* n, D3D10_VIEWPORT* vps) override
    {
        if (!vps)
        {
            *n = m_numViewports;
            return;
        }
        for (UINT i = 0; i < *n; i++)
            vps[i] = i < m_numViewports ? m_vp10[i] : D3D10_VIEWPORT{};
    }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* n, D3D10_RECT* rects) override
    {
        if (!rects)
        {
            *n = m_numScissors;
            return;
        }
        for (UINT i = 0; i < *n; i++)
            rects[i] = i < m_numScissors ? m_scissors[i] : D3D10_RECT{};
    }

    // ---- device misc
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override
    {
        return g.dev->GetDeviceRemovedReason();
    }
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT flags) override
    {
        m_exceptionMode = flags;
        return S_OK;
    }
    UINT STDMETHODCALLTYPE GetExceptionMode() override
    {
        return m_exceptionMode;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        return PrivateGet(this, guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        return PrivateSet(this, guid, size, data, nullptr);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
    {
        return PrivateSet(this, guid, 0, nullptr, data);
    }
    void STDMETHODCALLTYPE ClearState() override;
    void STDMETHODCALLTYPE Flush() override
    {
        Submit();
    }

    // ---- creation
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D10_BUFFER_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Buffer** out) override;
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D10_TEXTURE1D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture1D** out) override;
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D10_TEXTURE2D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture2D** out) override;
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D10_TEXTURE3D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture3D** out) override;
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D10Resource* r, const D3D10_SHADER_RESOURCE_VIEW_DESC* desc, ID3D10ShaderResourceView** out) override
    {
        D3D10_SHADER_RESOURCE_VIEW_DESC1 d1{};
        if (desc)
            memcpy(&d1, desc, sizeof(*desc));
        return CreateShaderResourceView1(r, desc ? &d1 : nullptr, reinterpret_cast<ID3D10ShaderResourceView1**>(out));
    }
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(ID3D10Resource* r, const D3D10_SHADER_RESOURCE_VIEW_DESC1* desc, ID3D10ShaderResourceView1** out) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D10Resource* r, const D3D10_RENDER_TARGET_VIEW_DESC* desc, ID3D10RenderTargetView** out) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D10Resource* r, const D3D10_DEPTH_STENCIL_VIEW_DESC* desc, ID3D10DepthStencilView** out) override;
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D10_INPUT_ELEMENT_DESC* elements, UINT n, const void* code, SIZE_T bytes, ID3D10InputLayout** out) override;
    template <class S, class I>
    HRESULT CreateShader(const void* code, SIZE_T bytes, I** out)
    {
        if (!code || !bytes)
            return E_INVALIDARG;
        if (!out)
            return S_FALSE;
        auto* s = new S;
        s->code.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + bytes);
        s->contentHash = HashBytes(code, bytes);
        s->Reflect();
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* code, SIZE_T bytes, ID3D10VertexShader** out) override
    {
        return CreateShader<VS12>(code, bytes, out);
    }
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* code, SIZE_T bytes, ID3D10GeometryShader** out) override
    {
        return CreateShader<GS12>(code, bytes, out);
    }
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D10_SO_DECLARATION_ENTRY*, UINT, UINT,
                                                                   ID3D10GeometryShader**) override
    {
        LogLimited("gsso", "CreateGeometryShaderWithStreamOutput is not supported");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* code, SIZE_T bytes, ID3D10PixelShader** out) override
    {
        return CreateShader<PS12>(code, bytes, out);
    }
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D10_BLEND_DESC* desc, ID3D10BlendState** out) override
    {
        if (!desc)
            return E_INVALIDARG;
        const D3D10_BLEND_DESC1 d1 = Blend1From(*desc);
        return CreateBlendState1(&d1, reinterpret_cast<ID3D10BlendState1**>(out));
    }
    HRESULT STDMETHODCALLTYPE CreateBlendState1(const D3D10_BLEND_DESC1* desc, ID3D10BlendState1** out) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D10_DEPTH_STENCIL_DESC* desc, ID3D10DepthStencilState** out) override;
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D10_RASTERIZER_DESC* desc, ID3D10RasterizerState** out) override;
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D10_SAMPLER_DESC* desc, ID3D10SamplerState** out) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D10_QUERY_DESC* desc, ID3D10Query** out) override;
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D10_QUERY_DESC*, ID3D10Predicate**) override
    {
        LogLimited("predicate", "CreatePredicate is not supported");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D10_COUNTER_DESC*, ID3D10Counter**) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT format, UINT* support) override
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        if (!support || FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
            return E_FAIL;
        *support = fs.Support1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels) override
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
        if (!levels)
            return E_INVALIDARG;
        *levels = SUCCEEDED(g.dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) ? q.NumQualityLevels : 0;
        return S_OK;
    }
    void STDMETHODCALLTYPE CheckCounterInfo(D3D10_COUNTER_INFO* info) override
    {
        if (info)
            *info = {};
    }
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D10_COUNTER_DESC*, D3D10_COUNTER_TYPE*, UINT*, char*, UINT*, char*, UINT*, char*, UINT*) override
    {
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetCreationFlags() override
    {
        return m_creationFlags;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE, REFIID, void**) override
    {
        LogLimited("shared", "OpenSharedResource is not supported");
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE SetTextFilterSize(UINT w, UINT h) override
    {
        m_textFilter[0] = w;
        m_textFilter[1] = h;
    }
    void STDMETHODCALLTYPE GetTextFilterSize(UINT* w, UINT* h) override
    {
        if (w)
            *w = m_textFilter[0];
        if (h)
            *h = m_textFilter[1];
    }
    D3D10_FEATURE_LEVEL1 STDMETHODCALLTYPE GetFeatureLevel() override
    {
        return D3D10_FEATURE_LEVEL_10_1;
    }
