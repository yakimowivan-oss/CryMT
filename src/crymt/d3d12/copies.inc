// CryMT D3D12 back end, class Device: barriers, copies and uploads.
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // ---- translation
    void Submit();
    void BeginList();
    void RestoreListState();
    // Pass names for frame debuggers (CRYMT_RENDERDOC or CRYMT_MARKERS): PIX events on the main list; names still open
    // at a submission are closed there and opened again on the next list.
    void PassName(const char* text);
    void PassNameEnd();
    void WaitIdle();
    bool Apply(bool indexed);
    bool ApplyOnce(bool indexed);
    ID3D12PipelineState* Pso(bool indexed);
    void ApplyStage(Stage s, uint32_t cbMask, UINT srvEnd, UINT samplerEnd);

    void Require(ResourceInfo* info, D3D12_RESOURCE_STATES needed)
    {
        if (info->buffer || info->state == needed)
            return;
        if (info->state == D3D12_RESOURCE_STATE_DEPTH_WRITE && needed == kSrvState)
            m_stats.depthToSrv++;
        NoteBarrier(info, info->state, needed);
        m_barriers.push_back(Transition(info->res, info->state, needed));
        info->state = needed;
    }

    void FlushBarriers()
    {
        if (m_barriers.empty())
            return;
        m_list->ResourceBarrier(static_cast<UINT>(m_barriers.size()), m_barriers.data());
        m_stats.barriers += m_barriers.size();
        m_stats.barrierCalls++;
        m_barriers.clear();
    }

    // A default buffer's page is read by this list. Pages start each list in COMMON (buffers decay at the end of every
    // ExecuteCommandLists) and are moved to GENERIC_READ explicitly, so a later copy into them knows the state to leave.
    void MarkRead(Buffer12* b)
    {
        if (BufferPage* p = b->Page())
            MarkPageRead(p);
    }

    void MarkPageRead(BufferPage* p)
    {
        if (p->listSerial != g_mainSerial)
        {
            p->listSerial = g_mainSerial;
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_GENERIC_READ));
            p->listState = kPageAllRead;
        }
        else if (p->listState == kPageCopyDest)
        {
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));
            p->listState = kPageAllRead;
        }
    }

    void PrepareCopyDest(BufferPage* p)
    {
        if (p->listSerial != g_mainSerial)
        {
            p->listSerial = g_mainSerial;
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
            p->listState = kPageCopyDest;
        }
        else if (p->listState == kPageAllRead)
        {
            NoteBarrier(nullptr, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
            m_barriers.push_back(Transition(p->res, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST));
            p->listState = kPageCopyDest;
        }
    }

    void CopyBuffer(Buffer12* dst, uint64_t dstOffset, Buffer12* src, uint64_t srcOffset, uint64_t bytes)
    {
        if (dst->Dynamic() || !dst->Res() || !src->Res() || !bytes)
        {
            LogLimited("copybuf", "buffer copy into a dynamic buffer is not supported");
            return;
        }
        if (dst->Page() && dst->Page() == src->Page())
        {
            LogLimited("copysame", "buffer copy within one page is not supported");
            return;
        }
        if (BufferPage* p = dst->Page())
            PrepareCopyDest(p);
        if (dst->Staging())
            dst->gpuWriteSerial = g_mainSerial;
        MarkRead(src);
        if (src->Staging())
            src->gpuReadSerial = g_mainSerial;
        FlushBarriers();
        m_list->CopyBufferRegion(dst->Res(), dst->Offset() + dstOffset, src->Res(), src->Offset() + srcOffset, bytes);
    }

    D3D12_TEXTURE_COPY_LOCATION Location(TextureCore* t, UINT sub)
    {
        D3D12_TEXTURE_COPY_LOCATION l{};
        l.pResource = t->res;
        if (t->Staging())
        {
            l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            l.PlacedFootprint = t->footprints[sub];
        }
        else
        {
            l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            l.SubresourceIndex = sub;
        }
        return l;
    }

    void CopyTexture(TextureCore* dst, UINT dstSub, UINT x, UINT y, UINT z, TextureCore* src, UINT srcSub, const D3D10_BOX* box)
    {
        if (dstSub >= dst->subresources || srcSub >= src->subresources)
            return;
        // An empty box copies nothing in D3D10; D3D12 calls it invalid (undefined behaviour). The texture streamer sends
        // such copies for BC textures (256x256 fmt 77, region 0x0x0 in the copy journal of a GPU hang, 2026-11-12).
        if (box && (box->right <= box->left || box->bottom <= box->top || box->back <= box->front))
        {
            g_emptyCopies++;
            return;
        }
        if (dst->Staging())
            dst->gpuWriteSerial = g_mainSerial;
        else
            Require(dst, D3D12_RESOURCE_STATE_COPY_DEST);
        if (src->Staging())
            src->gpuReadSerial = g_mainSerial;
        else
            Require(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        FlushBarriers();
        const D3D12_TEXTURE_COPY_LOCATION d = Location(dst, dstSub);
        const D3D12_TEXTURE_COPY_LOCATION s = Location(src, srcSub);
        if (dst->Staging() && src->Staging())
        {
            CopyStagingRows(dst, dstSub, x, y, z, src, srcSub, box);
            return;
        }
        NoteCopy("CopyTexture", d, x, y, z, s, reinterpret_cast<const D3D12_BOX*>(box));
        m_list->CopyTextureRegion(&d, x, y, z, &s, reinterpret_cast<const D3D12_BOX*>(box));
    }

    // A copy between two staging textures. Both are buffers here (each subresource at its copyable footprint), and D3D12's
    // CopyTextureRegion needs a texture on one side: footprint to footprint is invalid. The game's save thumbnail does
    // this (the screen read back into a staging texture, then a 256x72 region into another): the GPU hung on it, the
    // driver reset it (DRED and the copy journal of the user's hang 2026-11-12 19:14 and of rp_full_safe 15:27, both a
    // 256x72 footprint-to-footprint copy as the last one). So the region goes row by row with CopyBufferRegion (the
    // staging heap is custom, system memory: a valid copy source and destination).
    void CopyStagingRows(TextureCore* dst, UINT dstSub, UINT x, UINT y, UINT z, TextureCore* src, UINT srcSub, const D3D10_BOX* box)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& df = dst->footprints[dstSub];
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& sf = src->footprints[srcSub];
        const int f = sf.Footprint.Format;
        const bool bc = (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
        const UINT block = bc ? 4 : 1;
        const UINT bytes = bc ? ((f >= 70 && f <= 72) || (f >= 79 && f <= 81) ? 8 : 16) : FormatBytes(sf.Footprint.Format);
        const UINT left = box ? box->left : 0, top = box ? box->top : 0, front = box ? box->front : 0;
        const UINT right = box ? box->right : sf.Footprint.Width, bottom = box ? box->bottom : sf.Footprint.Height;
        const UINT back = box ? box->back : sf.Footprint.Depth;
        const UINT cols = (right - left + block - 1) / block, rows = (bottom - top + block - 1) / block;
        const UINT64 rowBytes = static_cast<UINT64>(cols) * bytes;
        const UINT srcRows = src->rows[srcSub], dstRows = dst->rows[dstSub]; // rows (of blocks) per slice
        UINT copies = 0;
        for (UINT slice = 0; slice < back - front; slice++)
            for (UINT r = 0; r < rows; r++)
            {
                const UINT sy = top / block + r, dy = y / block + r;
                if (sy >= srcRows || dy >= dstRows)
                    break;
                const UINT64 so = sf.Offset + (static_cast<UINT64>(front + slice) * srcRows + sy) * sf.Footprint.RowPitch + (left / block) * bytes;
                const UINT64 dO = df.Offset + (static_cast<UINT64>(z + slice) * dstRows + dy) * df.Footprint.RowPitch + (x / block) * bytes;
                const UINT64 room = std::min<UINT64>(sf.Footprint.RowPitch - (left / block) * bytes, df.Footprint.RowPitch - (x / block) * bytes);
                m_list->CopyBufferRegion(dst->res, dO, src->res, so, std::min(rowBytes, room));
                copies++;
            }
        g_stagingRowCopies += copies;
        LogLimited("stagingcopy", "staging to staging copy %ux%ux%u (format %d) as %u row copies", right - left, bottom - top, back - front, f, copies);
    }

    // Upload memory for this list (UpdateSubresource, dynamic textures).
    UploadAlloc FrameUpload(uint64_t bytes, uint64_t align)
    {
        if (bytes > m_frameRing.size / 4)
        {
            UploadAlloc a;
            a.res = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (a.res)
            {
                a.cpu = MapWhole(a.res);
                DeferRelease(a.res);
            }
            return a;
        }
        for (;;)
        {
            UploadAlloc a;
            if (m_frameRing.TryAlloc(bytes, align, a))
                return a;
            Submit();
            WaitFence(m_frameRing.OldestFence() ? m_frameRing.OldestFence() : g.signaled);
            PollFence();
            m_frameRing.Reclaim();
        }
    }

    void UploadPending(TextureCore* t, UINT sub, const TextureCore::Pending& p)
    {
        Require(t, D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers();
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = t->res;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = sub;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = p.up.res;
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = p.footprint;
        NoteCopy("UploadPending", d, 0, 0, 0, s, nullptr);
        m_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    }
