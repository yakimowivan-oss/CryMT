// CryMT D3D12 back end, class Device: captures and dumps.
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // The capture key's dumps (logs\play\<tag>_<name>.*): copies recorded where the textures' states are known (the
    // GI pass: the raw trace after the filter read it, the full-size GI after the upsample wrote it; the scene hand-over:
    // DLSS's input and output), read back once the frame capture has waited for the GPU anyway. Kinds: 0 HDR colour
    // (<name>.ppm with 1 - exp(-x), the engine's tone curve at exposure 1, and <name>.f16), 1 the full-size GI
    // (<name>_occ.ppm: a, the share of ambient removed, white = all; <name>_rgb.ppm: bounce x 64; <name>.f16), 2 the raw
    // trace (<name>_occ.ppm, thin features in red). .f16: width, height, RGBA half floats.
    static float Half(uint16_t h)
    {
        const uint32_t sgn = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
        uint32_t f;
        if (e == 0)
            f = m == 0 ? sgn << 31 : 0; // denormals as zero (tiny)
        else if (e == 31)
            f = (sgn << 31) | 0x7F800000u | (m << 13);
        else
            f = (sgn << 31) | ((e + 112u) << 23) | (m << 13);
        float r;
        memcpy(&r, &f, 4);
        return r;
    }

    struct PendingDump
    {
        ID3D12Resource* rb;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        std::string name;
        int kind;
    };
    std::vector<PendingDump> m_dumps;
    std::string m_dumpTag;

    // ---- Voxel snapshot (crymt_voxsnap <name>, measuring the grid's stability): right after a voxel update, levels
    // 0..kVoxGeomLevels-1 (the grid, the geometry stamps, the screen's "seen") read back; after the present's wait written
    // to logs\play\voxsnap_<name>.bin: "VXS1", n, levels, the GI frame, toWorld (16 floats), level 0's cell size, then per
    // level its first cell (3 ints, as the content stands) and n^3 texels (x fastest) of: opacity u8 (a x 255), light u8
    // (255 (1 - exp(-luminance))), the stamp u32 (frame + 1 << 8 | opacity byte), seen u8. tools\prof\voxsnap_diff.py.
    struct VoxSnapPart
    {
        ID3D12Resource* rb;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        int level, kind; // 0 the grid (RGBA16F), 1 the stamps (R32_UINT), 2 seen (R8_UINT)
    };
    std::vector<VoxSnapPart> m_voxSnap;
    std::string m_voxSnapName;
    float m_voxSnapToWorld[16] = {};
    int m_voxSnapGrid[kVoxGeomLevels][3] = {};
    uint64_t m_voxSnapFrame = 0;

    void RecordVoxSnap(const float* toWorld)
    {
        g_voxSnapWanted = false;
        {
            std::lock_guard<std::mutex> guard(g_voxSnapLock);
            m_voxSnapName = g_voxSnapName;
        }
        memcpy(m_voxSnapToWorld, toWorld, sizeof(m_voxSnapToWorld));
        m_voxSnapFrame = m_giFrame;
        for (int l = 0; l < kVoxGeomLevels; l++)
        {
            memcpy(m_voxSnapGrid[l], m_voxPrevGrid[l], sizeof(m_voxSnapGrid[l]));
            for (int kind = 0; kind < 3; kind++)
            {
                ID3D12Resource* res = kind == 0 ? m_vox[l] : kind == 1 ? m_voxOcc[l] : m_voxSeen[l];
                if (!res)
                    continue;
                const D3D12_RESOURCE_DESC rd = res->GetDesc();
                VoxSnapPart p{nullptr, {}, l, kind};
                UINT64 total = 0;
                g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &p.fp, nullptr, nullptr, &total);
                p.fp.Offset = 0;
                p.rb = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
                if (!p.rb)
                    continue;
                D3D12_RESOURCE_BARRIER b = Transition(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = p.rb;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = p.fp;
                src.pResource = res;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                NoteCopy("voxel snapshot", dst, 0, 0, 0, src, nullptr);
                m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                b = Transition(res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                m_list->ResourceBarrier(1, &b);
                m_voxSnap.push_back(p);
            }
        }
    }

    // After a wait for the GPU.
    void WriteVoxSnap()
    {
        const uint32_t n = kVoxN, levels = kVoxGeomLevels;
        std::string name;
        for (char ch : m_voxSnapName)
            name += isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' ? ch : '_';
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%svoxsnap_%s.bin", CryMTCapture("").c_str(), name.empty() ? "x" : name.c_str());
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") == 0 && f)
        {
            const float cell0 = Gi().voxelSize;
            fwrite("VXS1", 1, 4, f);
            fwrite(&n, 4, 1, f);
            fwrite(&levels, 4, 1, f);
            fwrite(&m_voxSnapFrame, 8, 1, f);
            fwrite(m_voxSnapToWorld, 4, 16, f);
            fwrite(&cell0, 4, 1, f);
            std::vector<uint8_t> a(n * n * n), lum(n * n * n), seen(n * n * n);
            std::vector<uint32_t> stamp(n * n * n);
            for (uint32_t l = 0; l < levels; l++)
            {
                std::fill(a.begin(), a.end(), 0);
                std::fill(lum.begin(), lum.end(), 0);
                std::fill(seen.begin(), seen.end(), 0);
                std::fill(stamp.begin(), stamp.end(), 0u);
                for (VoxSnapPart& p : m_voxSnap)
                {
                    void* mapped = nullptr;
                    if (p.level != static_cast<int>(l) || FAILED(p.rb->Map(0, nullptr, &mapped)) || !mapped)
                        continue;
                    const auto* base = static_cast<const uint8_t*>(mapped);
                    const size_t row = p.fp.Footprint.RowPitch, slice = row * p.fp.Footprint.Height;
                    for (uint32_t z = 0; z < n; z++)
                        for (uint32_t y = 0; y < n; y++)
                        {
                            const uint8_t* r = base + z * slice + y * row;
                            for (uint32_t x = 0; x < n; x++)
                            {
                                const size_t i = (static_cast<size_t>(z) * n + y) * n + x;
                                if (p.kind == 0)
                                {
                                    const auto* h = reinterpret_cast<const uint16_t*>(r + x * 8);
                                    const float alpha = std::min(std::max(Half(h[3]), 0.0f), 1.0f);
                                    const float y709 = 0.2126f * Half(h[0]) + 0.7152f * Half(h[1]) + 0.0722f * Half(h[2]);
                                    a[i] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
                                    lum[i] = static_cast<uint8_t>((1.0f - std::exp(-std::max(y709, 0.0f))) * 255.0f + 0.5f);
                                }
                                else if (p.kind == 1)
                                    stamp[i] = reinterpret_cast<const uint32_t*>(r)[x];
                                else
                                    seen[i] = r[x];
                            }
                        }
                    p.rb->Unmap(0, nullptr);
                }
                fwrite(m_voxSnapGrid[l], 4, 3, f);
                fwrite(a.data(), 1, a.size(), f);
                fwrite(lum.data(), 1, lum.size(), f);
                fwrite(stamp.data(), 4, stamp.size(), f);
                fwrite(seen.data(), 1, seen.size(), f);
            }
            fclose(f);
            Log("voxel snapshot %s: GI frame %llu, camera %.2f %.2f %.2f", path, static_cast<unsigned long long>(m_voxSnapFrame),
                m_voxSnapToWorld[12], m_voxSnapToWorld[13], m_voxSnapToWorld[14]);
        }
        else
            Log("voxel snapshot: cannot write %s", path);
        for (VoxSnapPart& p : m_voxSnap)
            DeferRelease(p.rb);
        m_voxSnap.clear();
    }
    bool m_giDumpWanted = false;

    // res is in COPY_SOURCE with the barriers flushed.
    // kind 6: an RGBK cube face (the engine's environment cube; sub = the face), written decoded as <name>.ppm.
    void RecordDump(ID3D12Resource* res, const char* name, int kind, UINT sub = 0)
    {
        if (!res)
            return;
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        const bool rg = rd.Format == DXGI_FORMAT_R16G16_FLOAT;
        const bool r8 = rd.Format == DXGI_FORMAT_R8_UNORM;
        const bool rgba8 = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const bool r16f = rd.Format == DXGI_FORMAT_R16_FLOAT;
        const bool r32f = rd.Format == DXGI_FORMAT_R32_FLOAT;
        const bool raw = kind == 11; // the texels as they are (GI v2's probe atlases)
        if (r16f && !raw)
            kind = 9; // one half per texel, raw (the DLSS bias mask)
        if (r32f && !raw)
            kind = 10; // one float per texel, raw (the GI's distances)
        if (rd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && rd.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS && !rg && !r8 && !rgba8 && !r16f &&
            !r32f)
        {
            Log("capture: %s format %d not dumped", name, static_cast<int>(rd.Format));
            return;
        }
        if (rg && !raw)
            kind = 3; // motion vectors
        if ((r8 || rgba8) && kind != 6 && !raw)
            kind = r8 ? 4 : 5; // one channel (the screen-space shadows; the shadow mask's red: the sun)
        PendingDump d{nullptr, {}, name, kind};
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&rd, sub, 1, 0, &d.fp, nullptr, nullptr, &total);
        d.fp.Offset = 0;
        d.rb = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!d.rb)
            return;
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = d.rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = d.fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = sub;
        NoteCopy("dump readback", dst, 0, 0, 0, src, nullptr);
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m_dumps.push_back(d);
    }

    // After a wait for the GPU (the frame capture's WaitIdle).
    void WriteDumps()
    {
        for (PendingDump& d : m_dumps)
        {
            void* p = nullptr;
            if (d.kind == 6 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // An RGBK cube face: rgb a 16 (HDRDecode_FromLDR_RGBK), shown as 1 - exp(-x); mean HDR and alpha in the log.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.ppm", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                fopen_s(&f, path, "wb");
                if (f)
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
                const int faceIndex = d.name.size() == 8 && d.name.compare(0, 7, "envcube") == 0 ? d.name[7] - '0' : -1;
                if (faceIndex >= 0 && faceIndex < 6)
                {
                    m_cubeFaces[faceIndex].assign(static_cast<size_t>(w) * h * 3, 0.0f);
                    m_cubeFaceSize = w;
                }
                double sum[4] = {};
                for (UINT y = 0; y < h; y++)
                {
                    const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch;
                    for (UINT x = 0; x < w; x++)
                    {
                        const float k = src[x * 4 + 3] / 255.0f * 16.0f;
                        for (int c = 0; c < 3; c++)
                        {
                            const float v = src[x * 4 + c] / 255.0f * k;
                            if (faceIndex >= 0 && faceIndex < 6 && w == h)
                                m_cubeFaces[faceIndex][(static_cast<size_t>(y) * w + x) * 3 + c] = v;
                            sum[c] += v;
                            row[x * 3 + c] = static_cast<uint8_t>(std::min(1.0f - std::exp(-v), 1.0f) * 255.0f);
                        }
                        sum[3] += src[x * 4 + 3] / 255.0;
                    }
                    if (f)
                        fwrite(row.data(), 1, row.size(), f);
                }
                if (f)
                    fclose(f);
                const double n = double(w) * h;
                Log("capture: %s mean HDR %.3f %.3f %.3f, alpha %.3f (%ux%u)", d.name.c_str(), sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, w, h);
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if ((d.kind == 4 || d.kind == 5) && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // One 8-bit channel: <name>.pgm (white = 1), its mean and the share over 0.5 in the log.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height, step = d.kind == 4 ? 1 : 4;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.pgm", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                fopen_s(&f, path, "wb");
                if (f)
                    fprintf(f, "P5\n%u %u\n255\n", w, h);
                std::vector<uint8_t> row(w);
                double sum = 0;
                uint64_t over = 0;
                for (UINT y = 0; y < h; y++)
                {
                    const uint8_t* src = static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch;
                    for (UINT x = 0; x < w; x++)
                    {
                        row[x] = src[x * step];
                        sum += row[x];
                        over += row[x] > 127;
                    }
                    if (f)
                        fwrite(row.data(), 1, w, f);
                }
                if (f)
                    fclose(f);
                Log("capture: %s mean %.4f, over 0.5 %.2f%% (%ux%u)", d.name.c_str(), sum / 255.0 / (double(w) * h), 100.0 * over / (double(w) * h), w, h);
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 11 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // Raw texels: <name>.raw (width, height, bytes a texel, then the rows), e.g. GI v2's probe atlases.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                const UINT bpp = d.fp.Footprint.Format == DXGI_FORMAT_R16G16_FLOAT ? 4 : 8;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.raw", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                if (fopen_s(&f, path, "wb") == 0 && f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                    fwrite(&bpp, 4, 1, f);
                    for (UINT y = 0; y < h; y++)
                        fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * bpp, f);
                    fclose(f);
                }
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 10 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // One channel of floats: <name>.r32 (width, height, then the rows).
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.r32", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                if (fopen_s(&f, path, "wb") == 0 && f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                    for (UINT y = 0; y < h; y++)
                        fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * 4, f);
                    fclose(f);
                }
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 9 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // One channel of halves: <name>.f16 (width, height, then the rows).
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.f16", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                if (fopen_s(&f, path, "wb") == 0 && f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                    for (UINT y = 0; y < h; y++)
                        fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * 2, f);
                    fclose(f);
                }
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (d.kind == 3 && SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                // Motion vectors (R16G16F, UV units): <name>.f16 as RG halves, and the mean in pixels in the log.
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                double sx = 0, sy = 0;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s%s_%s.f16", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                FILE* f = nullptr;
                fopen_s(&f, path, "wb");
                if (f)
                {
                    fwrite(&w, 4, 1, f);
                    fwrite(&h, 4, 1, f);
                }
                for (UINT y = 0; y < h; y++)
                {
                    const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch);
                    if (f)
                        fwrite(row, 1, static_cast<size_t>(w) * 4, f);
                    for (UINT x = 0; x < w; x++)
                    {
                        sx += Half(row[x * 2]);
                        sy += Half(row[x * 2 + 1]);
                    }
                }
                if (f)
                    fclose(f);
                Log("capture: %s mean motion %.3f, %.3f px", d.name.c_str(), sx / (double(w) * h) * w, sy / (double(w) * h) * h);
                d.rb->Unmap(0, nullptr);
                p = nullptr;
            }
            else if (SUCCEEDED(d.rb->Map(0, nullptr, &p)) && p)
            {
                const UINT w = d.fp.Footprint.Width, h = d.fp.Footprint.Height;
                const auto at = [&](UINT x, UINT y, int c) {
                    return Half(*reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch + x * 8 + c * 2));
                };
                const auto ppm = [&](const char* suffix, auto pixel) {
                    char path[MAX_PATH];
                    snprintf(path, sizeof(path), "%s%s_%s%s.ppm", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str(), suffix);
                    FILE* f = nullptr;
                    if (fopen_s(&f, path, "wb") != 0 || !f)
                        return;
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                    std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
                    for (UINT y = 0; y < h; y++)
                    {
                        for (UINT x = 0; x < w; x++)
                            pixel(x, y, &row[x * 3]);
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                };
                const auto byte = [](float v) { return static_cast<uint8_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f); };
                if (d.kind == 7)
                {
                    // raw only (the motion blur target of a motion vector sequence)
                }
                else if (d.kind == 0)
                    ppm("", [&](UINT x, UINT y, uint8_t* o) {
                        for (int c = 0; c < 3; c++)
                        {
                            const float v = at(x, y, c);
                            o[c] = v == v ? byte(1.0f - std::exp(-v)) : 255;
                        }
                    });
                else
                    ppm("_occ", [&](UINT x, UINT y, uint8_t* o) {
                        const float a = at(x, y, 3);
                        const bool thin = a < -0.5f;
                        const uint8_t v = byte(thin ? -1.0f - a : a);
                        o[0] = thin ? 255 : v;
                        o[1] = thin ? 0 : v;
                        o[2] = thin ? 0 : v;
                    });
                if (d.kind == 1)
                    ppm("_rgb", [&](UINT x, UINT y, uint8_t* o) {
                        for (int c = 0; c < 3; c++)
                            o[c] = byte(at(x, y, c) * 0.25f); // bounce x 64 (of 255)
                    });
                if (d.kind != 2)
                {
                    char path[MAX_PATH];
                    snprintf(path, sizeof(path), "%s%s_%s.f16", CryMTCapture("").c_str(), m_dumpTag.c_str(), d.name.c_str());
                    FILE* f = nullptr;
                    if (fopen_s(&f, path, "wb") == 0 && f)
                    {
                        fwrite(&w, 4, 1, f);
                        fwrite(&h, 4, 1, f);
                        for (UINT y = 0; y < h; y++)
                            fwrite(static_cast<const uint8_t*>(p) + static_cast<size_t>(y) * d.fp.Footprint.RowPitch, 1, static_cast<size_t>(w) * 8, f);
                        fclose(f);
                    }
                }
                d.rb->Unmap(0, nullptr);
            }
            d.rb->Release();
        }
        if (!m_dumps.empty())
            Log("capture: %zu dumps written (%s)", m_dumps.size(), m_dumpTag.c_str());
        m_dumps.clear();
        WriteCubePicture();
    }

    // The engine's cube as one picture (<tag>_envcube.bmp, Windows opens it): what one standing at the cube's place sees
    // looking +X, -X, +Y (top row), -Y, up, down (bottom row), each labelled; the sides with Z up (horizon level, sky on
    // top), up and down with +Y at the top. Every pixel is the cube looked up by its world direction as the water does
    // (D3D cube addressing), so a face stored turned or mirrored shows so. Exposure from the cube's log-average (0.18,
    // Reinhard, gamma 2.2), as a photo; the HDR is RGBK-decoded.
    std::vector<float> m_cubeFaces[6];
    UINT m_cubeFaceSize = 0;
    void WriteCubePicture()
    {
        const UINT s = m_cubeFaceSize;
        for (auto& face : m_cubeFaces)
            if (!s || face.size() != static_cast<size_t>(s) * s * 3)
                return;
        static const char* const kGlyphs[5][7] = {
            {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}, // +
            {".....", ".....", ".....", "#####", ".....", ".....", "....."}, // -
            {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}, // X
            {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}, // Y
            {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}, // Z
        };
        const UINT gap = 8, W = s * 3 + gap * 4, H = s * 2 + gap * 3, scale = std::max(2u, s / 64);
        std::vector<uint8_t> img(static_cast<size_t>(W) * H * 3, 40);
        double logSum = 0;
        for (const auto& face : m_cubeFaces)
            for (size_t i = 0; i < face.size(); i += 3)
                logSum += std::log(1e-4 + 0.2126 * face[i] + 0.7152 * face[i + 1] + 0.0722 * face[i + 2]);
        const float exposure = static_cast<float>(0.18 / std::exp(logSum / (6.0 * s * s)));
        // Per panel: forward and up (right = forward x up).
        static const float kView[6][6] = {{1, 0, 0, 0, 0, 1}, {-1, 0, 0, 0, 0, 1}, {0, 1, 0, 0, 0, 1}, {0, -1, 0, 0, 0, 1}, {0, 0, 1, 0, 1, 0}, {0, 0, -1, 0, 1, 0}};
        for (int f = 0; f < 6; f++)
        {
            const UINT ox = gap + (f % 3) * (s + gap), oy = gap + (f / 3) * (s + gap);
            const float* fw = kView[f];
            const float* up = kView[f] + 3;
            const float rt[3] = {fw[1] * up[2] - fw[2] * up[1], fw[2] * up[0] - fw[0] * up[2], fw[0] * up[1] - fw[1] * up[0]};
            for (UINT y = 0; y < s; y++)
                for (UINT x = 0; x < s; x++)
                {
                    const float a = 2.0f * (x + 0.5f) / s - 1.0f, bb = 1.0f - 2.0f * (y + 0.5f) / s;
                    float d[3] = {fw[0] + a * rt[0] + bb * up[0], fw[1] + a * rt[1] + bb * up[1], fw[2] + a * rt[2] + bb * up[2]};
                    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
                    if (ay >= ax && ay >= az) // the engine's faces are flipped vertically (as Water.cfx looks them up)
                        d[2] = -d[2];
                    else
                        d[1] = -d[1];
                    int face;
                    float sc, tc;
                    if (ax >= ay && ax >= az)
                        face = d[0] > 0 ? 0 : 1, sc = (d[0] > 0 ? -d[2] : d[2]) / ax, tc = -d[1] / ax;
                    else if (ay >= az)
                        face = d[1] > 0 ? 2 : 3, sc = d[0] / ay, tc = (d[1] > 0 ? d[2] : -d[2]) / ay;
                    else
                        face = d[2] > 0 ? 4 : 5, sc = (d[2] > 0 ? d[0] : -d[0]) / az, tc = -d[1] / az;
                    const float wide = g_engineCubeTanHalf.load(); // the engine's faces see wider than 90 degrees
                    const UINT u = std::min(s - 1, static_cast<UINT>(std::max(0.0f, (sc / wide + 1) * 0.5f * s)));
                    const UINT v = std::min(s - 1, static_cast<UINT>(std::max(0.0f, (tc / wide + 1) * 0.5f * s)));
                    const float* hdr = &m_cubeFaces[face][(static_cast<size_t>(v) * s + u) * 3];
                    uint8_t* p = &img[(static_cast<size_t>(oy + y) * W + ox + x) * 3];
                    for (int c = 0; c < 3; c++)
                    {
                        const float e = hdr[c] * exposure;
                        p[c] = static_cast<uint8_t>(255.0f * std::pow(e / (1.0f + e), 1.0f / 2.2f));
                    }
                }
            const int glyphs[2] = {f % 2 == 0 ? 0 : 1, 2 + f / 2};
            for (int g = 0; g < 2; g++)
                for (UINT gy = 0; gy < 9 * scale; gy++)
                    for (UINT gx = 0; gx < 7 * scale; gx++)
                    {
                        const int cx = static_cast<int>(gx / scale) - 1, cy = static_cast<int>(gy / scale) - 1;
                        const bool on = cx >= 0 && cx < 5 && cy >= 0 && cy < 7 && kGlyphs[glyphs[g]][cy][cx] == '#';
                        uint8_t* p = &img[(static_cast<size_t>(oy + 4 + gy) * W + ox + 4 + g * 7 * scale + gx) * 3];
                        p[0] = on ? 255 : 0; // RGB here, BGR on writing
                        p[1] = on ? 255 : 0;
                        p[2] = on ? 0 : 0;
                    }
            m_cubeFaces[f].clear();
        }
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s%s_envcube.bmp", CryMTCapture("").c_str(), m_dumpTag.c_str());
        FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") != 0 || !file)
            return;
        const UINT rowBytes = (W * 3 + 3) & ~3u, size = 54 + rowBytes * H;
        uint8_t header[54] = {'B', 'M'};
        const auto put = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
        put(2, size);
        put(10, 54);
        put(14, 40);
        put(18, W);
        put(22, H);
        header[26] = 1;
        header[28] = 24;
        put(34, rowBytes * H);
        fwrite(header, 1, 54, file);
        std::vector<uint8_t> row(rowBytes, 0);
        for (UINT y = H; y-- > 0;)
        {
            for (UINT x = 0; x < W; x++)
            {
                const uint8_t* p = &img[(static_cast<size_t>(y) * W + x) * 3];
                row[x * 3] = p[2];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = p[0];
            }
            fwrite(row.data(), 1, rowBytes, file);
        }
        fclose(file);
        Log("engine cube picture: %s", path);
    }
