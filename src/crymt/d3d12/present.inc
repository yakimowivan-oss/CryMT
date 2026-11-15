// CryMT D3D12 back end: frame captures, Device::PresentFrame, the swap chain.
// Included in D3D12Backend.cpp, where this text stood.
// ---------------------------------------------------------------------------------------------------------------------
// Presentation: the renderer draws into our back buffer texture; Present copies it into the flip-model swap chain's
// current buffer. At most kFramesInFlight frames are queued.
// ---------------------------------------------------------------------------------------------------------------------
// Frame capture for image comparisons (CRYMT_CAPTURE_SEC=seconds after start, CRYMT_CAPTURE_COUNT, CRYMT_CAPTURE_TAG):
// consecutive back buffers copied to readback memory and written as PPM files to logs\play, without touching the window.
// CRYMT_CAPTURE_SEC may list several windows ("45,60"); window w writes <tag>_w<w>_c<n>.ppm. CRYMT_CAPTURE_FRAME lists
// world frames instead (CryMT's frame counter: a timedemo shows the same camera at the same frame in every run).
struct Capture
{
    std::vector<DWORD> starts;
    bool byFrame = false;
    size_t window = 0;
    int count = 0, done = 0;
    char tag[64] = "capture";
    std::vector<ID3D12Resource*> buffers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
};
Capture g_capture;
// The capture key (CryMT.cpp, PgUp with CRYMT_RECORDKEYS=auto): main asks for that many frames from the next present;
// they go to logs\play\key_MMDD_HHMMSS_w1_c<n>.ppm. Ignored while a capture runs.
std::atomic<int> g_captureKeyFrames{0};
std::atomic<int> g_captureKeyFlags{0}; // what was on, for the name: 1 GI, 2 the DLAA jitter, 4 object velocities
// Scene hand-overs left to dump DLSS's input and output (the capture key: 1; with CRYMT_MVSEQ=1 every captured frame, for
// the motion vector check tools\prof\mv_check.py: colour, vectors, the engine's object velocities, the foliage mask).
std::atomic<int> g_sceneDumpFrames{0};
int MvSequenceLevel()
{
    static const int level = [] {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"CRYMT_MVSEQ", v, 8) ? _wtoi(v) : 0;
    }();
    return level;
}
bool MvSequence()
{
    return MvSequenceLevel() != 0;
}

// Milliseconds since the process started (capture windows line up with CryMT's comparison schedule).
DWORD ProcessMilliseconds()
{
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<DWORD>((ticks(now) - ticks(created)) / 10000);
}

void CaptureInit()
{
    wchar_t text[256];
    if (GetEnvironmentVariableW(L"CRYMT_CAPTURE_TAG", text, 64))
        snprintf(g_capture.tag, sizeof(g_capture.tag), "%ls", text);
    g_capture.byFrame = GetEnvironmentVariableW(L"CRYMT_CAPTURE_FRAME", text, 256) != 0;
    if (!g_capture.byFrame && !GetEnvironmentVariableW(L"CRYMT_CAPTURE_SEC", text, 256))
        return;
    for (const wchar_t* p = text; *p;)
    {
        g_capture.starts.push_back(static_cast<DWORD>(_wtof(p) * (g_capture.byFrame ? 1.0 : 1000.0)));
        while (*p && *p != L',')
            p++;
        if (*p == L',')
            p++;
    }
    g_capture.count = GetEnvironmentVariableW(L"CRYMT_CAPTURE_COUNT", text, 64) ? _wtoi(text) : 8;
}

// Every frame, reduced (CRYMT_CAPTURE_STREAM=n: 1/n of the size each way, box filter), appended to
// logs\play\<tag>_stream.raw: width and height (uint32), then per frame the world frame (uint32) and RGB rows. For
// counting one-frame flicker over a whole demo; a frame is read back kFramesInFlight frames later, when it is done.
struct CaptureStream
{
    int scale = 0;
    FILE* file = nullptr;
    UINT width = 0, height = 0, written = 0;
    ID3D12Resource* buffers[kFramesInFlight] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kFramesInFlight] = {};
    uint32_t worldFrames[kFramesInFlight] = {};
    bool filled[kFramesInFlight] = {};
};
CaptureStream g_stream;

void CaptureStreamInit()
{
    wchar_t text[16];
    if (GetEnvironmentVariableW(L"CRYMT_CAPTURE_STREAM", text, 16))
        g_stream.scale = std::max(1, _wtoi(text));
}

void CaptureStreamWrite(size_t slot)
{
    CaptureStream& s = g_stream;
    s.filled[slot] = false;
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = s.footprints[slot];
    const UINT w = fp.Footprint.Width / s.scale, h = fp.Footprint.Height / s.scale;
    if (!s.file)
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s%s_stream.raw", CryMTCapture("").c_str(), g_capture.tag);
        if (fopen_s(&s.file, path, "wb") != 0 || !s.file)
        {
            s.scale = 0;
            return;
        }
        s.width = w;
        s.height = h;
        fwrite(&s.width, 4, 1, s.file);
        fwrite(&s.height, 4, 1, s.file);
    }
    void* p = nullptr;
    if (w != s.width || h != s.height || FAILED(s.buffers[slot]->Map(0, nullptr, &p)) || !p)
        return;
    const bool bgra = fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const UINT n = static_cast<UINT>(s.scale), area = n * n;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3);
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++)
        {
            UINT sum[3] = {};
            for (UINT dy = 0; dy < n; dy++)
            {
                const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y * n + dy) * fp.Footprint.RowPitch + x * n * 4;
                for (UINT dx = 0; dx < n; dx++, src += 4)
                {
                    sum[0] += src[bgra ? 2 : 0];
                    sum[1] += src[1];
                    sum[2] += src[bgra ? 0 : 2];
                }
            }
            uint8_t* dst = &out[(static_cast<size_t>(y) * w + x) * 3];
            for (int c = 0; c < 3; c++)
                dst[c] = static_cast<uint8_t>(sum[c] / area);
        }
    s.buffers[slot]->Unmap(0, nullptr);
    fwrite(&s.worldFrames[slot], 4, 1, s.file);
    fwrite(out.data(), 1, out.size(), s.file);
    if (++s.written % 64 == 0)
        fflush(s.file);
}

void CaptureWrite(int index)
{
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = g_capture.footprints[index];
    void* p = nullptr;
    g_capture.buffers[index]->Map(0, nullptr, &p);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s_w%zu_c%d.ppm", CryMTCapture("").c_str(), g_capture.tag, g_capture.window + 1, index + 1);
    FILE* f = nullptr;
    if (p && fopen_s(&f, path, "wb") == 0 && f)
    {
        fprintf(f, "P6\n%u %u\n255\n", fp.Footprint.Width, fp.Footprint.Height);
        std::vector<uint8_t> row(fp.Footprint.Width * 3);
        const bool bgra = fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        for (UINT y = 0; y < fp.Footprint.Height; y++)
        {
            const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * fp.Footprint.RowPitch;
            for (UINT x = 0; x < fp.Footprint.Width; x++)
            {
                row[x * 3] = src[x * 4 + (bgra ? 2 : 0)];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + (bgra ? 0 : 2)];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    g_capture.buffers[index]->Unmap(0, nullptr);
}

HRESULT Device::PresentFrame(IDXGISwapChain3* chain, ID3D12Resource* target, TextureCore* backbuffer, UINT sync, UINT flags)
{
    m_note.frame = m_frame;
    m_notes[m_frame % 16] = m_note;
    if (CopyLog() && m_frame % 600 == 300)
    {
        Log("pre-DLSS copies, frame %llu: %u (hand-over in [%s])", static_cast<unsigned long long>(m_frame), m_preCopyCount, m_handOverPass.c_str());
        for (uint32_t i = 0; i < m_preCopyCount; i++)
        {
            const auto* t = static_cast<const TextureCore*>(m_preCopies[i]);
            Log("  copy %u: %p %ux%u fmt %d, %s%s", i, static_cast<const void*>(t), t->width, t->height, static_cast<int>(t->format),
                m_preCopyWhere[i].c_str(), m_preCopyReads[i].empty() ? "\n    not read after DLSS" : m_preCopyReads[i].c_str());
        }
    }
    m_note = FrameNote{};
    m_preCopyCount = 0;
    GiApplyControl();
    RunVoxelView(backbuffer);
    if (g_stream.scale)
    {
        // This slot's last frame is done (the wait at the end of the present kFramesInFlight - 1 frames ago).
        const size_t slot = m_frame % kFramesInFlight;
        if (g_stream.filled[slot])
            CaptureStreamWrite(slot);
        D3D12_RESOURCE_DESC rd = backbuffer->Desc12();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = g_stream.footprints[slot];
        if (g_stream.buffers[slot] && (fp.Footprint.Width != rd.Width || fp.Footprint.Height != rd.Height))
        {
            g_stream.buffers[slot]->Release();
            g_stream.buffers[slot] = nullptr;
        }
        if (!g_stream.buffers[slot])
        {
            UINT64 total = 0;
            g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            g_stream.buffers[slot] = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (g_stream.buffers[slot] && (fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM || fp.Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM))
        {
            Require(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_stream.buffers[slot];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = backbuffer->res;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            NoteCopy("stream capture", dst, 0, 0, 0, src, nullptr);
            m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            g_stream.worldFrames[slot] = static_cast<uint32_t>(::CryMTWorldFrame());
            g_stream.filled[slot] = true;
        }
    }
    if (const int keyFrames = g_captureKeyFrames.exchange(0); keyFrames > 0 && g_capture.done == 0 && g_capture.buffers.empty())
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        const int flags = g_captureKeyFlags.load();
        snprintf(g_capture.tag, sizeof(g_capture.tag), "key_%02u%02u_%02u%02u%02u_gi%d_gtao%d_cs%d_bk%d", t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, flags & 1, (flags >> 1) & 1, (flags >> 2) & 1, (flags >> 3) & 15);
        g_capture.byFrame = true;
        g_capture.count = keyFrames;
        g_capture.starts.assign(1, 0);
        g_capture.window = 0;
        m_dumpTag = g_capture.tag;
        m_giDumpWanted = true;
        m_engineCubeDumpWanted = true;
        m_sceneDumpIndex = 0;
        g_sceneDumpFrames = MvSequence() ? std::max(keyFrames - 1, 1) : 1; // the scenes before the capture's last present
    }
    if (g_capture.window < g_capture.starts.size() &&
        (g_capture.byFrame ? ::CryMTWorldFrame() : ProcessMilliseconds()) >= g_capture.starts[g_capture.window])
    {
        D3D12_RESOURCE_DESC rd = backbuffer->Desc12();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ID3D12Resource* buffer = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        if (buffer)
        {
            Require(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = buffer;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = backbuffer->res;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            NoteCopy("capture", dst, 0, 0, 0, src, nullptr);
            m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            g_capture.buffers.push_back(buffer);
            g_capture.footprints.push_back(fp);
        }
        g_capture.done++;
        if (g_capture.done == g_capture.count)
        {
            WaitIdle();
            for (size_t i = 0; i < g_capture.buffers.size(); i++)
            {
                CaptureWrite(static_cast<int>(i));
                g_capture.buffers[i]->Release();
            }
            Log("capture: window %zu, %zu frames written", g_capture.window + 1, g_capture.buffers.size());
            for (uint64_t k = m_frame >= 10 ? m_frame - 10 : 0; k < m_frame; k++)
            {
                const FrameNote& n = m_notes[k % 16];
                if (n.frame == k)
                    Log("capture: frame %llu: dlss %u gi %u, after DLSS: colour read %u, draws into it %u (with depth %u); copies of the "
                        "scene before DLSS %u, read after %u, given DLSS's picture %u; jitter told %+.3f %+.3f",
                        static_cast<unsigned long long>(k), n.dlss, n.gi, n.colourReads, n.lateDraws, n.late3D, n.copies, n.copyReads, n.fixed, n.jx, n.jy);
            }
            WriteDumps();
            g_capture.buffers.clear();
            g_capture.footprints.clear();
            g_capture.done = 0;
            g_capture.window++;
        }
    }
    bool cubeDump = false;
    if (g_cubeDumpWanted.exchange(false))
    {
        if (m_engineCube && m_dumps.empty())
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char tag[48];
            snprintf(tag, sizeof(tag), "cube_%02u%02u_%02u%02u%02u", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            m_dumpTag = tag;
            Require(m_engineCube->info, D3D12_RESOURCE_STATE_COPY_SOURCE);
            FlushBarriers();
            static const char* const faces[6] = {"envcube0", "envcube1", "envcube2", "envcube3", "envcube4", "envcube5"};
            for (UINT face = 0; face < 6; face++)
                RecordDump(m_engineCube->info->res, faces[face], 6, face);
            if (m_hdrCube.res) // the HDR copy the water reads, level 0: <tag>_hdrcube<face>.ppm (D3D layout, 1 - exp(-x))
            {
                Require(&m_hdrCube, D3D12_RESOURCE_STATE_COPY_SOURCE);
                FlushBarriers();
                static const char* const hdrFaces[6] = {"hdrcube0", "hdrcube1", "hdrcube2", "hdrcube3", "hdrcube4", "hdrcube5"};
                for (UINT face = 0; face < 6; face++)
                    RecordDump(m_hdrCube.res, hdrFaces[face], 0, face * m_hdrCube.mips);
                Require(&m_hdrCube, kSrvState);
                FlushBarriers();
            }
            cubeDump = true;
        }
        else
            Log("engine cube picture: no engine cube now (water reflection CUBE MAP or SSR + CUBE MAP with the engine's cube)");
    }
    Require(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_barriers.push_back(Transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
    FlushBarriers();
    m_list->CopyResource(target, backbuffer->res);
    m_barriers.push_back(Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
    Submit();
    const HRESULT hr = chain->Present(sync, flags);
    if (FAILED(hr) && !m_removedLogged)
    {
        Log("Present failed %08lx, device removed reason %08lx", static_cast<unsigned long>(hr), static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
        m_removedLogged = true;
        LogDred();
    }
    if (cubeDump)
    {
        WaitIdle();
        WriteDumps();
    }
    if (!m_voxSnap.empty())
    {
        WaitIdle();
        WriteVoxSnap();
    }
    m_frameFences[m_frame % kFramesInFlight] = g.signaled;
    m_frame++;
    m_stats.frames++;
    g_presented++;
    if (g_statsResetAsked.exchange(false))
    {
        m_stats = Stats{};
        std::fill(m_passTicks.begin(), m_passTicks.end(), 0);
        m_passStampsDropped = 0;
    }
    // Buffer memory over a long session (a heap that only grows ends in the engine's "Pipeline buffer overflow. Current
    // geometry cannot fit in video memory" when a page can no longer be created).
    if (m_frame % 3600 == 0)
    {
        // With the video memory the process uses against the budget Windows gives it (DXGI, local = the card's own).
        static IDXGIAdapter3* a3;
        if (!a3)
            g.factory->EnumAdapterByLuid(g.dev->GetAdapterLuid(), __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3));
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (a3)
        {
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
        }
        static uint64_t envBindsBefore, env2DBefore;
        static uint64_t ssrPrevBefore;
        static uint64_t partDrawsBefore;
        Log("environment map binds per frame: %.1f (2D maps %.1f); water SSR on the previous frame %.1f, particle layer draws %.1f",
            (g_device->m_envBinds - envBindsBefore) / 3600.0, (g_device->m_env2DBinds - env2DBefore) / 3600.0,
            (g_device->m_ssrPrevBinds - ssrPrevBefore) / 3600.0, (g_device->m_partDraws - partDrawsBefore) / 3600.0);
        ssrPrevBefore = g_device->m_ssrPrevBinds;
        partDrawsBefore = g_device->m_partDraws;
        envBindsBefore = g_device->m_envBinds;
        env2DBefore = g_device->m_env2DBinds;
        Log("heaps at frame %llu: video memory %.0f of %.0f MB, shared %.0f MB; upload pages %llu (+%llu dedicated), %.1f MB in free lists; "
            "default pages %llu (+%llu dedicated), %.1f MB in free lists",
            static_cast<unsigned long long>(m_frame), local.CurrentUsage / 1048576.0, local.Budget / 1048576.0, shared.CurrentUsage / 1048576.0,
            static_cast<unsigned long long>(g_slices.pages), static_cast<unsigned long long>(g_slices.dedicated), g_slices.FreeBytes() / 1048576.0,
            static_cast<unsigned long long>(g_blocks.pages), static_cast<unsigned long long>(g_blocks.dedicated), g_blocks.FreeBytes() / 1048576.0);
    }
    WaitFence(m_frameFences[m_frame % kFramesInFlight]);
    if (GetTickCount() - g_pipes.savedAt > 5000)
    {
        SavePipelines();
        g_pipes.savedAt = GetTickCount();
    }
    return hr;
}

constexpr UINT kSwapBuffers = 3;
std::atomic<uint32_t> g_screenWidth, g_screenHeight; // the back buffer, for the camera jitter

DXGI_FORMAT SwapFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return f;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

class SwapChain12 final : public IDXGISwapChain
{
public:
    IDXGISwapChain3* chain = nullptr;
    DXGI_SWAP_CHAIN_DESC desc{}; // as the renderer asked, with the current size
    UINT flags = 0;              // the real swap chain's flags
    bool windowed = true;
    bool borderless = false;     // fullscreen requests go to the borderless window (Window.h); the chain stays windowed
    ID3D12Resource* buffers[kSwapBuffers] = {};
    Texture2D12* backbuffer = nullptr;

    bool CreateBuffers()
    {
        DXGI_SWAP_CHAIN_DESC1 d1{};
        chain->GetDesc1(&d1);
        desc.BufferDesc.Width = d1.Width;
        desc.BufferDesc.Height = d1.Height;
        g_screenWidth = d1.Width;
        g_screenHeight = d1.Height;
        for (UINT i = 0; i < kSwapBuffers; i++)
            if (FAILED(chain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffers[i]))))
                return false;
        D3D10_TEXTURE2D_DESC td{};
        td.Width = d1.Width;
        td.Height = d1.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = desc.BufferDesc.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D10_USAGE_DEFAULT;
        td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
        ID3D10Texture2D* tex = nullptr;
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &tex)))
            return false;
        backbuffer = static_cast<Texture2D12*>(tex);
        return true;
    }

    void ReleaseBuffers()
    {
        for (ID3D12Resource*& b : buffers)
            if (b)
            {
                b->Release();
                b = nullptr;
            }
        if (backbuffer)
        {
            backbuffer->Release();
            backbuffer = nullptr;
        }
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDXGIObject)) || IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain)))
        {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        LogGuid("swap chain", riid);
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_refs.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = m_refs.fetch_sub(1) - 1;
        if (refs == 0)
        {
            g_device->WaitIdle();
            ReleaseBuffers();
            if (!windowed)
                chain->SetFullscreenState(FALSE, nullptr);
            chain->Release();
            PrivateErase(this);
            delete this;
        }
        return refs;
    }

    // ---- IDXGIObject, IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        return PrivateSet(this, guid, size, data, nullptr);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override
    {
        return PrivateSet(this, guid, 0, nullptr, object);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        return PrivateGet(this, guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override
    {
        return chain->GetParent(riid, parent);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override
    {
        return g_device->QueryInterface(riid, device);
    }

    // ---- IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT presentFlags) override
    {
        if (presentFlags & DXGI_PRESENT_TEST)
            return chain->Present(sync, presentFlags);
        if (windowed && sync == 0 && (flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
            presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
        return g_device->PresentFrame(chain, buffers[chain->GetCurrentBackBufferIndex()], backbuffer, sync, presentFlags);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID riid, void** surface) override
    {
        return backbuffer ? backbuffer->QueryInterface(riid, surface) : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(WINBOOL fullscreen, IDXGIOutput* target) override
    {
        if (borderless)
        {
            BorderlessSetFullscreen(desc.OutputWindow, fullscreen != FALSE);
            Log("SetFullscreenState(%d): borderless", fullscreen);
            return S_OK;
        }
        g_device->WaitIdle();
        const HRESULT hr = chain->SetFullscreenState(fullscreen, target);
        if (SUCCEEDED(hr))
            windowed = !fullscreen;
        Log("SetFullscreenState(%d) %08lx", fullscreen, static_cast<unsigned long>(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(WINBOOL* fullscreen, IDXGIOutput** target) override
    {
        if (!borderless)
            return chain->GetFullscreenState(fullscreen, target);
        if (fullscreen)
            *fullscreen = BorderlessFullscreen();
        if (target)
            *target = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override
    {
        if (!d)
            return E_INVALIDARG;
        *d = desc;
        d->Windowed = borderless ? !BorderlessFullscreen() : windowed;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT width, UINT height, DXGI_FORMAT format, UINT) override
    {
        g_device->WaitIdle();
        ReleaseBuffers();
        if (format != DXGI_FORMAT_UNKNOWN)
            desc.BufferDesc.Format = format;
        const HRESULT hr = chain->ResizeBuffers(kSwapBuffers, width, height, format != DXGI_FORMAT_UNKNOWN ? SwapFormat(format) : DXGI_FORMAT_UNKNOWN, flags);
        if (!CreateBuffers())
            Log("ResizeBuffers: buffers not recreated");
        Log("ResizeBuffers %ux%u fmt %d: %08lx, now %ux%u", width, height, format, static_cast<unsigned long>(hr), desc.BufferDesc.Width,
            desc.BufferDesc.Height);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override
    {
        return borderless && BorderlessFullscreen() ? S_OK : chain->ResizeTarget(mode);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override
    {
        return chain->GetContainingOutput(output);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override
    {
        return chain->GetFrameStatistics(stats);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override
    {
        return chain->GetLastPresentCount(count);
    }

private:
    std::atomic<ULONG> m_refs{1};
};

// ---------------------------------------------------------------------------------------------------------------------
// Start-up.
// ---------------------------------------------------------------------------------------------------------------------
bool InitCore(IDXGIAdapter* engineAdapter)
{
    rdoc::Load(); // before the device and the factory, so RenderDoc sees them
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d12 || !dxgi)
    {
        Log("d3d12.dll or dxgi.dll not found");
        return false;
    }
    const auto create = reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    const auto getDebug = reinterpret_cast<GetDebugInterfaceFn>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
    const auto serialize = reinterpret_cast<SerializeRootSignatureFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
    g_serializeRoot = serialize;
    const auto createFactory = reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
    if (!create || !serialize || !createFactory)
    {
        Log("D3D12 or DXGI 1.3 entry points missing");
        return false;
    }
    const bool debug = GetEnvironmentVariableW(L"CRYMT_D3D12DEBUG", nullptr, 0) != 0;
    if (debug && getDebug)
    {
        ID3D12Debug* d = nullptr;
        if (SUCCEEDED(getDebug(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&d))))
        {
            d->EnableDebugLayer();
            d->Release();
            Log("debug layer enabled");
        }
    }
    // DRED (device removed extended data): after a GPU hang or page fault LogDred writes the unfinished command lists' last
    // operations and the faulting address with the allocations there. Page faults always; auto-breadcrumbs unless
    // CRYMT_DRED=0 (the user's GPU hang of 2026-11-12 left only "device removed 887a0006" behind).
    if (getDebug)
    {
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        if (SUCCEEDED(getDebug(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), reinterpret_cast<void**>(&dred))))
        {
            wchar_t v[8] = {};
            const bool crumbs = !(GetEnvironmentVariableW(L"CRYMT_DRED", v, 8) && wcscmp(v, L"0") == 0);
            dred->SetAutoBreadcrumbsEnablement(crumbs ? D3D12_DRED_ENABLEMENT_FORCED_ON : D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
            Log("DRED: page faults on, breadcrumbs %s", crumbs ? "on" : "off");
        }
    }
    if (FAILED(createFactory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&g.factory))))
    {
        Log("CreateDXGIFactory2 failed");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    if (engineAdapter)
    {
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(engineAdapter->GetDesc(&ad)))
        {
            g.factory->EnumAdapterByLuid(ad.AdapterLuid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
            Log("adapter: %ls", ad.Description);
        }
    }
    const HRESULT hr = create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g.dev));
    if (adapter)
        adapter->Release();
    if (FAILED(hr))
    {
        Log("D3D12CreateDevice failed %08lx", static_cast<unsigned long>(hr));
        return false;
    }
    wchar_t dlssVar[8] = {};
    if (GetEnvironmentVariableW(L"CRYMT_DLSS", dlssVar, 8) != 0 && wcscmp(dlssVar, L"0") != 0 && rdoc::Loaded())
        Log("DLSS off: RenderDoc is in the process");
    else if (GetEnvironmentVariableW(L"CRYMT_DLSS", dlssVar, 8) != 0 && wcscmp(dlssVar, L"0") != 0) // 0: off
        dlss::Init(g.dev, g.dev->GetAdapterLuid(), [](const char* text) { Log("%s", text); });
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (FAILED(dlss::CreateQueue(g.dev, &qd, &g.queue)) ||
        FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g.fence))))
    {
        Log("queue or fence creation failed");
        return false;
    }
    g.srvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.rtvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g.dsvInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    g.samplerInc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    g_srvCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    g_srvCpu.inc = g.srvInc;
    g_rtvCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_rtvCpu.inc = g.rtvInc;
    g_dsvCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    g_dsvCpu.inc = g.dsvInc;
    g_samplerCpu.type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    g_samplerCpu.inc = g.samplerInc;
    if (!CreateRootSignature(serialize))
        return false;
    g_nullSrv = g_srvCpu.Alloc();
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    g.dev->CreateShaderResourceView(nullptr, &nullSrv, g_nullSrv);
    g_defaultSampler = g_samplerCpu.Alloc();
    D3D12_SAMPLER_DESC sd{};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    g.dev->CreateSampler(&sd, g_defaultSampler);
    if (debug && SUCCEEDED(g.dev->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&g.info))))
    {
        // Clears without an optimized clear value (the renderer's clear colors vary): expected, not an error.
        // Depth-only passes keep a pixel shader that writes a color target: D3D10 discards the writes too.
        D3D12_MESSAGE_ID deny[] = {static_cast<D3D12_MESSAGE_ID>(820), static_cast<D3D12_MESSAGE_ID>(821), static_cast<D3D12_MESSAGE_ID>(679)};
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = 3;
        filter.DenyList.pIDList = deny;
        g.info->AddStorageFilterEntries(&filter);
    }
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(g.factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5))))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            g.tearing = allow != FALSE;
        f5->Release();
    }
    g.reflect = reinterpret_cast<ReflectShaderFn>(GetProcAddress(GetModuleHandleW(L"d3d10.dll"), "D3D10ReflectShader"));
    g_initRing.Init(128u << 20);
    if (!g_initRing.res)
        return false;
    OpenPipelines();
    CaptureInit();
    CaptureStreamInit();
    Log("D3D12 device ready: tearing %d, reflection %d", g.tearing, g.reflect != nullptr);
    return true;
}
} // namespace
} // namespace dx12

