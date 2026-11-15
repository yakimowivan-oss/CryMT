// CryMT D3D12 back end: Device: creating resources and states, recognizing CryMT's placeholder textures.
// Included in D3D12Backend.cpp, where this text stood.
// ---------------------------------------------------------------------------------------------------------------------
// Updates and dynamic textures.
// ---------------------------------------------------------------------------------------------------------------------
void Device::UpdateSubresource(ID3D10Resource* dst, UINT sub, const D3D10_BOX* box, const void* data, UINT rowPitch, UINT depthPitch)
{
    if (!dst || !data)
        return;
    if (Buffer12* b = AsBuffer(dst))
    {
        if (!b->Page())
        {
            LogLimited("updbuf", "UpdateSubresource on a dynamic or staging buffer is not supported");
            return;
        }
        const UINT from = box ? box->left : 0;
        const UINT bytes = box ? box->right - box->left : b->desc.ByteWidth;
        if (!bytes)
            return;
        const UploadAlloc up = FrameUpload(bytes, 16);
        if (!up.cpu)
            return;
        memcpy(up.cpu, data, bytes);
        if (b->desc.BindFlags & D3D10_BIND_CONSTANT_BUFFER)
        {
            if (from == 0 && bytes >= sizeof(b->head))
            {
                memcpy(b->head, data, sizeof(b->head));
                b->headKnown = true;
            }
            else if (from < sizeof(b->head))
                b->headKnown = false;
        }
        PrepareCopyDest(b->Page());
        FlushBarriers();
        m_list->CopyBufferRegion(b->Res(), b->Offset() + from, up.res, up.offset, bytes);
        return;
    }
    TextureCore* t = AsTexture(dst);
    if (!t || t->Staging() || sub >= t->subresources)
        return;
    const UINT mip = sub % t->mips;
    UINT w = std::max(1u, t->width >> mip);
    UINT h = t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D ? 1 : std::max(1u, t->height >> mip);
    UINT d = t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? std::max(1u, t->depthOrArray >> mip) : 1;
    UINT x = 0, y = 0, z = 0;
    if (box)
    {
        x = box->left;
        y = box->top;
        z = box->front;
        w = box->right - box->left;
        h = box->bottom - box->top;
        d = box->back - box->front;
    }
    if (!w || !h || !d)
        return;
    const bool bc = BlockCompressed(t->format);
    const UINT rows = bc ? (h + 3) / 4 : h;
    const UINT rowBytes = bc ? (w + 3) / 4 * BlockBytes(t->format) : w * FormatBytes(t->format);
    const UINT pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UploadAlloc up = FrameUpload(static_cast<uint64_t>(pitch) * rows * d, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!up.cpu)
        return;
    for (UINT zz = 0; zz < d; zz++)
        for (UINT r = 0; r < rows; r++)
            memcpy(up.cpu + (static_cast<uint64_t>(zz) * rows + r) * pitch,
                   static_cast<const uint8_t*>(data) + static_cast<uint64_t>(zz) * depthPitch + static_cast<uint64_t>(r) * rowPitch, rowBytes);
    Require(t, D3D12_RESOURCE_STATE_COPY_DEST);
    FlushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = t->res;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = up.res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = up.offset;
    sl.PlacedFootprint.Footprint = {t->format, bc ? (w + 3) & ~3u : w, bc ? (h + 3) & ~3u : h, d, pitch};
    NoteCopy("UpdateSubresource", dl, x, y, z, sl, nullptr);
    m_list->CopyTextureRegion(&dl, x, y, z, &sl, nullptr);
}

HRESULT TextureCore::MapSub(UINT sub, D3D10_MAP type, UINT flags, void** data, UINT* rowPitch, UINT* depthPitch)
{
    if (!data || sub >= subresources)
        return E_INVALIDARG;
    if (Staging())
    {
        const HRESULT hr = WaitStaging(this, type == D3D10_MAP_READ || type == D3D10_MAP_READ_WRITE, flags);
        if (FAILED(hr))
            return hr;
        *data = stagingCpu + footprints[sub].Offset;
        if (rowPitch)
            *rowPitch = footprints[sub].Footprint.RowPitch;
        if (depthPitch)
            *depthPitch = footprints[sub].Footprint.RowPitch * rows[sub];
        return S_OK;
    }
    if (usage != D3D10_USAGE_DYNAMIC || type != D3D10_MAP_WRITE_DISCARD)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC d = Desc12();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT n = 0;
    UINT64 rowSize = 0, total = 0;
    g.dev->GetCopyableFootprints(&d, sub, 1, 0, &fp, &n, &rowSize, &total);
    const UploadAlloc up = g_device->FrameUpload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!up.cpu)
        return E_OUTOFMEMORY;
    fp.Offset = up.offset;
    pending[sub] = {up, fp, true};
    *data = up.cpu;
    if (rowPitch)
        *rowPitch = fp.Footprint.RowPitch;
    if (depthPitch)
        *depthPitch = fp.Footprint.RowPitch * n;
    return S_OK;
}

void TextureCore::UnmapSub(UINT sub)
{
    if (Staging() || sub >= pending.size() || !pending[sub].active)
        return;
    pending[sub].active = false;
    g_device->UploadPending(this, sub, pending[sub]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Creation.
// ---------------------------------------------------------------------------------------------------------------------
HRESULT Device::CreateBuffer(const D3D10_BUFFER_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Buffer** out)
{
    if (!desc || !desc->ByteWidth)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* b = new Buffer12;
    b->desc = *desc;
    const void* initData = init ? init->pSysMem : nullptr;
    if (desc->Usage == D3D10_USAGE_DYNAMIC)
    {
        b->slice = g_slices.Alloc(desc->ByteWidth);
        b->prod = b->slice;
        g_buffer12Vtable = *reinterpret_cast<void* const*>(static_cast<ID3D10Buffer*>(b));
        if (!b->slice.cpu)
        {
            Log("CreateBuffer: no upload memory for a dynamic buffer of %u bytes (pages %llu), device removed reason %08lx", desc->ByteWidth,
                static_cast<unsigned long long>(g_slices.pages), static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
            b->Release();
            return E_OUTOFMEMORY;
        }
        if (initData)
            memcpy(b->slice.cpu, initData, desc->ByteWidth);
    }
    else if (desc->Usage == D3D10_USAGE_STAGING)
    {
        b->res = CreateStagingBuffer(desc->ByteWidth);
        if (!b->res)
        {
            b->Release();
            return E_OUTOFMEMORY;
        }
        void* p = nullptr;
        b->res->Map(0, nullptr, &p);
        b->stagingCpu = static_cast<uint8_t*>(p);
        if (initData)
            memcpy(b->stagingCpu, initData, desc->ByteWidth);
    }
    else
    {
        if (initData && (desc->BindFlags & D3D10_BIND_CONSTANT_BUFFER) && desc->ByteWidth >= sizeof(b->head))
        {
            memcpy(b->head, initData, sizeof(b->head));
            b->headKnown = true;
        }
        b->block = g_blocks.Alloc(desc->ByteWidth);
        if (!b->block.page)
        {
            Log("CreateBuffer: no default buffer memory for %u bytes (pages %llu), device removed reason %08lx", desc->ByteWidth,
                static_cast<unsigned long long>(g_blocks.pages), static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
            b->Release();
            return E_OUTOFMEMORY;
        }
        if (initData)
        {
            std::lock_guard<std::recursive_mutex> guard(g.lock);
            const UploadAlloc up = InitUpload(desc->ByteWidth, 16);
            if (up.cpu)
            {
                memcpy(up.cpu, initData, desc->ByteWidth);
                UploadList()->CopyBufferRegion(b->block.page->res, b->block.offset, up.res, up.offset, desc->ByteWidth);
            }
        }
    }
    *out = b;
    return S_OK;
}

void CopyInit(uint8_t* base, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, const UINT* rows, const UINT64* rowBytes,
              const D3D10_SUBRESOURCE_DATA* init, UINT n)
{
    for (UINT s = 0; s < n; s++)
    {
        const auto* src = static_cast<const uint8_t*>(init[s].pSysMem);
        if (!src)
            continue;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = layouts[s];
        for (UINT z = 0; z < fp.Footprint.Depth; z++)
            for (UINT r = 0; r < rows[s]; r++)
                memcpy(base + fp.Offset + (static_cast<uint64_t>(z) * rows[s] + r) * fp.Footprint.RowPitch,
                       src + static_cast<uint64_t>(z) * init[s].SysMemSlicePitch + static_cast<uint64_t>(r) * init[s].SysMemPitch, rowBytes[s]);
    }
}

HRESULT FinishTexture(TextureCore* t, const D3D10_SUBRESOURCE_DATA* init)
{
    const bool is3D = t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D;
    if (!t->mips)
    {
        UINT size = std::max(t->width, std::max(t->height, is3D ? t->depthOrArray : 1u));
        t->mips = 1;
        while (size > 1)
        {
            size >>= 1;
            t->mips++;
        }
    }
    t->subresources = t->mips * (is3D ? 1 : t->depthOrArray);
    const D3D12_RESOURCE_DESC d = t->Desc12();
    const UINT n = t->subresources;
    if (t->Staging())
    {
        t->footprints.resize(n);
        t->rows.resize(n);
        std::vector<UINT64> rowBytes(n);
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&d, 0, n, 0, t->footprints.data(), t->rows.data(), rowBytes.data(), &total);
        t->res = CreateStagingBuffer(total);
        if (!t->res)
            return E_OUTOFMEMORY;
        void* p = nullptr;
        t->res->Map(0, nullptr, &p);
        t->stagingCpu = static_cast<uint8_t*>(p);
        if (init)
            CopyInit(t->stagingCpu, t->footprints.data(), t->rows.data(), rowBytes.data(), init, n);
        return S_OK;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES state = (t->bind & D3D10_BIND_DEPTH_STENCIL)   ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                  : (t->bind & D3D10_BIND_RENDER_TARGET) ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                                         : D3D12_RESOURCE_STATE_COPY_DEST;
    if (init)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    // Optimized clear values: every depth clear the renderer makes is to 1, colour targets get 0. (Measured on this
    // NVIDIA driver: no GPU time difference either way; kept as the API asks.)
    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* clearValue = nullptr;
    if (t->bind & D3D10_BIND_DEPTH_STENCIL)
    {
        clear.Format = DepthViewFormat(t->format);
        clear.DepthStencil.Depth = 1.0f;
        clearValue = &clear;
    }
    else if ((t->bind & D3D10_BIND_RENDER_TARGET) && !TypelessColor(t->format))
    {
        clear.Format = t->format;
        clearValue = &clear;
    }
    const HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clearValue, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void**>(&t->res));
    if (FAILED(hr))
    {
        Log("CreateCommittedResource texture %ux%ux%u mips %u fmt %d bind 0x%x failed %08lx", t->width, t->height, t->depthOrArray, t->mips, t->format,
            t->bind, static_cast<unsigned long>(hr));
        return hr;
    }
    t->state = state;
    if (t->usage == D3D10_USAGE_DYNAMIC)
        t->pending.resize(n);
    if (init)
    {
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(n);
        std::vector<UINT> rows(n);
        std::vector<UINT64> rowBytes(n);
        UINT64 total = 0;
        g.dev->GetCopyableFootprints(&d, 0, n, 0, layouts.data(), rows.data(), rowBytes.data(), &total);
        std::lock_guard<std::recursive_mutex> guard(g.lock);
        const UploadAlloc up = InitUpload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!up.cpu)
            return E_OUTOFMEMORY;
        CopyInit(up.cpu, layouts.data(), rows.data(), rowBytes.data(), init, n);
        ID3D12GraphicsCommandList* list = UploadList();
        for (UINT s = 0; s < n; s++)
        {
            D3D12_TEXTURE_COPY_LOCATION dl{};
            dl.pResource = t->res;
            dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dl.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION sl{};
            sl.pResource = up.res;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sl.PlacedFootprint = layouts[s];
            sl.PlacedFootprint.Offset += up.offset;
            NoteCopy("initial data", dl, 0, 0, 0, sl, nullptr);
            list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        }
        const D3D12_RESOURCE_STATES after = (t->bind & D3D10_BIND_SHADER_RESOURCE)   ? kSrvState
                                            : (t->bind & D3D10_BIND_RENDER_TARGET) ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                                                   : D3D12_RESOURCE_STATE_COPY_DEST;
        if (after != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            const D3D12_RESOURCE_BARRIER b = Transition(t->res, D3D12_RESOURCE_STATE_COPY_DEST, after);
            list->ResourceBarrier(1, &b);
        }
        t->state = after;
    }
    return S_OK;
}

template <class T, class Desc>
void FillTexture(T* t, const Desc& desc, D3D10_RESOURCE_DIMENSION dim, UINT width, UINT height, UINT depthOrArray, UINT samples, UINT quality)
{
    t->desc = desc;
    t->dim = dim;
    t->usage = desc.Usage;
    t->bind = desc.BindFlags;
    t->cpuAccess = desc.CPUAccessFlags;
    t->misc = desc.MiscFlags;
    t->format = desc.Format;
    t->width = width;
    t->height = height;
    t->depthOrArray = depthOrArray;
    t->mips = desc.MipLevels;
    t->samples = samples;
    t->quality = quality;
}

HRESULT Device::CreateTexture1D(const D3D10_TEXTURE1D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture1D** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* t = new Texture1D12;
    FillTexture(t, *desc, D3D10_RESOURCE_DIMENSION_TEXTURE1D, desc->Width, 1, desc->ArraySize, 1, 0);
    const HRESULT hr = FinishTexture(t, init);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    t->desc.MipLevels = t->mips;
    *out = t;
    return S_OK;
}

// Textures/CryMT/gi.dds: CryMT's own 4x1 all-zero placeholder, the texture of the GI sampler in the patched fragLib.cfi
// and terrain.cfx (CryMT_GIAmbient: ambient * (1 - a) + rgb). The engine loads and binds it like any texture; pixel
// shaders see CryMT's GI texture in its place, and without CryMT's back end its zeros leave the ambient as it was.
// The materials' environment cube maps (envMapSamplerCUBE = $EnvironmentCubeMap in the game's shaders: all 1824
// Environment slots of the shipped .mtl files are files such as textures/cubemaps/beach_blur.dds): cube textures created
// with data and only read by shaders. Render-target cubes (the engine's own dynamic ones, light cubes) are not.
bool IsEnvCube(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    return (d.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE) && init && init->pSysMem && d.BindFlags == D3D10_BIND_SHADER_RESOURCE &&
           d.Usage != D3D10_USAGE_DYNAMIC && d.Width >= 8;
}

// Stage 0 of the dynamic environment maps, a test: CRYMT_ENVTEST=RRGGBB replaces every environment cube map with one
// cube of that colour, so which materials read them shows on screen.
uint32_t EnvTestColour()
{
    static int colour = -2;
    if (colour == -2)
    {
        char v[16] = {};
        colour = GetEnvironmentVariableA("CRYMT_ENVTEST", v, sizeof(v)) ? static_cast<int>(strtoul(v, nullptr, 16) & 0xFFFFFF) : -1;
    }
    return static_cast<uint32_t>(colour);
}

// Textures/CryMT/env.dds (CryMTSettings): a 4x4 cube, black, face 0's second texel E7000000 (the mark, alpha only).
bool IsEnvPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || !(d.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE) || d.Width != 4 || d.Height != 4 || d.ArraySize != 6)
        return false;
    const auto* p = static_cast<const uint32_t*>(init->pSysMem);
    return p[0] == 0 && p[1] == 0xE7000000u;
}

bool IsGiPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 4 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 16; i++)
        if (p[i] != 0)
            return false;
    return true;
}

// Textures/CryMT/jitter.dds: CryMT's 16x1 placeholder for the frame texture (patched ShadowMaskGen.cfx): texels 0..14
// zero (what the shader reads: no jitter, no contact shadows without CryMT), the last 5A5A5A5A (the mark).
bool IsJitterPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 16 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 64; i++)
        if (p[i] != (i < 60 ? 0 : 0x5A))
            return false;
    return true;
}

// Textures/CryMT/ssrpart.dds: CryMT's 2x1 placeholder for the previous frame's particle layer (the water's SSR): the first
// texel zero, the second 4F4F4F4F (the mark).
bool IsSsrPartPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x4F))
            return false;
    return true;
}

// Textures/CryMT/ssrprev.dds: CryMT's 2x1 placeholder for the previous frame's scene (the water's SSR, patched Water.cfx):
// the first texel zero, the second 4E4E4E4E (the mark).
bool IsSsrPrevPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x4E))
            return false;
    return true;
}

// Textures/CryMT/foliage.dds: CryMT's 2x1 placeholder for the foliage mask (patched ShadowMaskGen.cfx): the first
// texel zero, the second A5A5A5A5 (the mark).
bool IsFoliagePlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0xA5))
            return false;
    return true;
}

// Textures/CryMT/pvol.dds: CryMT's 2x1 placeholder for the particles' occlusion volume (patched Particles.cfi): the
// first texel zero (a cell size of 0: no change), the second 3C3C3C3C (the mark).
bool IsPvolPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x3C))
            return false;
    return true;
}

// Textures/CryMT/vfog.dds: CryMT's 4x1x2 volume of zeros for the GI in the fog (patched PostProcess.cfx: zeros leave the
// engine's fog as it is). Told by its size and format (its texels too when the engine hands them over).
bool IsVfogPlaceholder(const D3D10_TEXTURE3D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (d.Width != 4 || d.Height != 1 || d.Depth != 2 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    if (init && init->pSysMem)
        for (UINT z = 0; z < 2; z++)
            for (UINT i = 0; i < 16; i++)
                if (static_cast<const uint8_t*>(init->pSysMem)[z * init->SysMemSlicePitch + i])
                    return false;
    return true;
}

// Textures/CryMT/fplus.dds: CryMT's 2x1 placeholder for the Forward+ test's tile lists (patched fragLib.cfi): the first
// texel zero (off), the second 6B6B6B6B (the mark).
bool IsFPlusPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i < 4 ? 0 : 0x6B))
            return false;
    return true;
}

// Textures/CryMT/sss.dds: CryMT's 2x1 placeholder for the screen-space shadows (patched ShadowMaskGen.cfx, fragLib.cfi):
// the first texel zero (no shadow), the second 005C005C (the mark; red zero in either byte order).
bool IsSssPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i == 5 || i == 7 ? 0x5C : 0))
            return false;
    return true;
}

// Textures/CryMT/hiz.dds: CryMT's 2x1 placeholder for the water's depth pyramid (patched Water.cfx): red zero in both
// texels (no surface), the second texel 007C007C (the mark).
bool IsHizPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 1 || d.ArraySize != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i == 5 || i == 7 ? 0x7C : 0))
            return false;
    return true;
}

// Textures/CryMT/waterenv.dds: CryMT's placeholder for the water's reflection cube (patched Water.cfx): a 2x2 cube, alpha 0
// everywhere, face 0's second texel 003D3D3D (the mark).
bool IsWaterEnvPlaceholder(const D3D10_TEXTURE2D_DESC& d, const D3D10_SUBRESOURCE_DATA* init)
{
    if (!init || !init->pSysMem || d.Width != 2 || d.Height != 2 || d.ArraySize != 6 || !(d.MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE))
        return false;
    const auto* p = static_cast<const uint8_t*>(init->pSysMem);
    for (int i = 0; i < 8; i++)
        if (p[i] != (i >= 4 && i < 7 ? 0x3D : 0))
            return false;
    return true;
}

HRESULT Device::CreateTexture2D(const D3D10_TEXTURE2D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture2D** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* t = new Texture2D12;
    FillTexture(t, *desc, D3D10_RESOURCE_DIMENSION_TEXTURE2D, desc->Width, desc->Height, desc->ArraySize, desc->SampleDesc.Count, desc->SampleDesc.Quality);
    const HRESULT hr = FinishTexture(t, init);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    if (IsEnvPlaceholder(*desc, init))
    {
        g_envPlaceholder = Info(t);
        Log("environment placeholder texture recognized");
    }
    else if (IsWaterEnvPlaceholder(*desc, init))
    {
        g_waterEnvPlaceholder = Info(t);
        Log("water reflection cube placeholder texture recognized");
    }
    else if (IsEnvCube(*desc, init))
    {
        Info(t)->envCube = true;
        static int logged;
        if (logged++ < 64)
            Log("environment cube %u: %ux%u, %u mips, format %d", logged, desc->Width, desc->Height, desc->MipLevels, static_cast<int>(desc->Format));
    }
    else if (desc->MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE)
    {
        static int logged;
        if (logged++ < 32)
            Log("other cube texture: %ux%u, %u mips, format %d, bind 0x%x, usage %d, data %d", desc->Width, desc->Height, desc->MipLevels,
                static_cast<int>(desc->Format), desc->BindFlags, static_cast<int>(desc->Usage), init && init->pSysMem ? 1 : 0);
    }
    if (IsGiPlaceholder(*desc, init))
    {
        g_giPlaceholder = Info(t);
        Log("GI placeholder texture recognized");
    }
    else if (IsJitterPlaceholder(*desc, init))
    {
        g_jitterPlaceholder = Info(t);
        Log("jitter placeholder texture recognized");
    }
    else if (IsFoliagePlaceholder(*desc, init))
    {
        g_foliagePlaceholder = Info(t);
        Log("foliage placeholder texture recognized");
    }
    else if (IsPvolPlaceholder(*desc, init))
    {
        g_pvolPlaceholder = Info(t);
        Log("particle volume placeholder texture recognized");
    }
    else if (IsFPlusPlaceholder(*desc, init))
    {
        g_fplusPlaceholder = Info(t);
        Log("Forward+ placeholder texture recognized");
    }
    else if (IsSssPlaceholder(*desc, init))
    {
        g_sssPlaceholder = Info(t);
        Log("screen-space shadows placeholder texture recognized");
    }
    else if (IsHizPlaceholder(*desc, init))
    {
        g_hizPlaceholder = Info(t);
        Log("depth pyramid placeholder texture recognized");
    }
    else if (IsSsrPrevPlaceholder(*desc, init))
    {
        g_ssrPrevPlaceholder = Info(t);
        Log("water previous-frame placeholder texture recognized");
    }
    else if (IsSsrPartPlaceholder(*desc, init))
    {
        g_ssrPartPlaceholder = Info(t);
        Log("water particle-layer placeholder texture recognized");
    }
    else if (desc->Width == 4 && desc->Height <= 4 && Gi().on)
    {
        int zero = -1;
        if (init && init->pSysMem && (desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc->Format == DXGI_FORMAT_B8G8R8A8_UNORM))
        {
            zero = 1;
            for (UINT y = 0; y < desc->Height; y++)
                for (UINT x = 0; x < 16; x++)
                    if (static_cast<const uint8_t*>(init->pSysMem)[y * init->SysMemPitch + x])
                        zero = 0;
        }
        Log("small texture %ux%u fmt %d mips %u usage %d bind 0x%x init %d zero %d", desc->Width, desc->Height, desc->Format, desc->MipLevels,
            desc->Usage, desc->BindFlags, init && init->pSysMem ? 1 : 0, zero);
    }
    t->desc.MipLevels = t->mips;
    *out = t;
    return S_OK;
}

HRESULT Device::CreateTexture3D(const D3D10_TEXTURE3D_DESC* desc, const D3D10_SUBRESOURCE_DATA* init, ID3D10Texture3D** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* t = new Texture3D12;
    FillTexture(t, *desc, D3D10_RESOURCE_DIMENSION_TEXTURE3D, desc->Width, desc->Height, desc->Depth, 1, 0);
    const HRESULT hr = FinishTexture(t, init);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    if (IsVfogPlaceholder(*desc, init))
    {
        g_vfogPlaceholder = Info(t);
        Log("volumetric fog placeholder texture recognized");
    }
    t->desc.MipLevels = t->mips;
    *out = t;
    return S_OK;
}

// The view D3D10 makes when no description is given: the whole resource in its own format.
D3D10_SHADER_RESOURCE_VIEW_DESC1 DefaultSrv(const TextureCore* t)
{
    D3D10_SHADER_RESOURCE_VIEW_DESC1 d{};
    d.Format = t->format;
    const UINT arrays = t->depthOrArray;
    int dim;
    if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
    {
        dim = arrays > 1 ? D3D10_SRV_DIMENSION_TEXTURE1DARRAY : D3D10_SRV_DIMENSION_TEXTURE1D;
        d.Texture1DArray = {0, t->mips, 0, arrays};
    }
    else if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D)
    {
        dim = D3D10_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D = {0, t->mips};
    }
    else if (t->samples > 1)
    {
        dim = arrays > 1 ? D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D10_SRV_DIMENSION_TEXTURE2DMS;
        d.Texture2DMSArray = {0, arrays};
    }
    else if (t->misc & D3D10_RESOURCE_MISC_TEXTURECUBE)
    {
        dim = arrays > 6 ? D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY : D3D10_SRV_DIMENSION_TEXTURECUBE;
        d.TextureCubeArray = {0, t->mips, 0, arrays / 6};
    }
    else
    {
        dim = arrays > 1 ? D3D10_SRV_DIMENSION_TEXTURE2DARRAY : D3D10_SRV_DIMENSION_TEXTURE2D;
        d.Texture2DArray = {0, t->mips, 0, arrays};
    }
    d.ViewDimension = static_cast<D3D10_SRV_DIMENSION1>(dim);
    return d;
}

HRESULT Device::CreateShaderResourceView1(ID3D10Resource* r, const D3D10_SHADER_RESOURCE_VIEW_DESC1* desc, ID3D10ShaderResourceView1** out)
{
    ResourceInfo* info = Info(r);
    if (!info || (info->buffer && !desc))
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    const D3D10_SHADER_RESOURCE_VIEW_DESC1 d1 = desc ? *desc : DefaultSrv(static_cast<TextureCore*>(info));
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = d1.Format;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.ViewDimension = static_cast<D3D12_SRV_DIMENSION>(d1.ViewDimension);
    switch (static_cast<int>(d1.ViewDimension))
    {
    case D3D10_SRV_DIMENSION_BUFFER:
        d.Buffer.FirstElement = d1.Buffer.FirstElement;
        d.Buffer.NumElements = d1.Buffer.NumElements;
        break;
    case D3D10_SRV_DIMENSION_TEXTURE1D:
        d.Texture1D = {d1.Texture1D.MostDetailedMip, d1.Texture1D.MipLevels, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE1DARRAY:
        d.Texture1DArray = {d1.Texture1DArray.MostDetailedMip, d1.Texture1DArray.MipLevels, d1.Texture1DArray.FirstArraySlice, d1.Texture1DArray.ArraySize, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2D:
        d.Texture2D = {d1.Texture2D.MostDetailedMip, d1.Texture2D.MipLevels, 0, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2DARRAY:
        d.Texture2DArray = {d1.Texture2DArray.MostDetailedMip, d1.Texture2DArray.MipLevels, d1.Texture2DArray.FirstArraySlice, d1.Texture2DArray.ArraySize, 0,
                            0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY:
        d.Texture2DMSArray = {d1.Texture2DMSArray.FirstArraySlice, d1.Texture2DMSArray.ArraySize};
        break;
    case D3D10_SRV_DIMENSION_TEXTURE3D:
        d.Texture3D = {d1.Texture3D.MostDetailedMip, d1.Texture3D.MipLevels, 0.0f};
        break;
    case D3D10_SRV_DIMENSION_TEXTURECUBE:
        d.TextureCube = {d1.TextureCube.MostDetailedMip, d1.TextureCube.MipLevels, 0.0f};
        break;
    case D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY:
        d.TextureCubeArray = {d1.TextureCubeArray.MostDetailedMip, d1.TextureCubeArray.MipLevels, d1.TextureCubeArray.First2DArrayFace,
                              d1.TextureCubeArray.NumCubes, 0.0f};
        break;
    default:
        return E_INVALIDARG;
    }
    auto* v = new Srv12;
    r->AddRef();
    v->resource = r;
    v->info = info;
    v->desc = d1;
    v->d12 = d;
    v->heap = &g_srvCpu;
    v->cpu = g_srvCpu.Alloc();
    if (info->buffer)
    {
        auto* b = static_cast<Buffer12*>(info);
        D3D12_SHADER_RESOURCE_VIEW_DESC bd = d;
        bd.Buffer.FirstElement += b->Offset() / FormatBytes(d.Format);
        g.dev->CreateShaderResourceView(b->Res(), &bd, v->cpu);
        v->generation = b->generation;
    }
    else
        g.dev->CreateShaderResourceView(info->res, &d, v->cpu);
    *out = v;
    return S_OK;
}

HRESULT Device::CreateRenderTargetView(ID3D10Resource* r, const D3D10_RENDER_TARGET_VIEW_DESC* desc, ID3D10RenderTargetView** out)
{
    TextureCore* t = r ? AsTexture(r) : nullptr;
    if (!t)
    {
        LogLimited("rtvbuf", "render target views of buffers are not supported");
        return E_INVALIDARG;
    }
    if (!out)
        return S_FALSE;
    D3D10_RENDER_TARGET_VIEW_DESC d1{};
    if (desc)
        d1 = *desc;
    else
    {
        d1.Format = t->format;
        if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_RTV_DIMENSION_TEXTURE1DARRAY : D3D10_RTV_DIMENSION_TEXTURE1D, d1.Texture1DArray = {0, 0, t->depthOrArray};
        else if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D)
            d1.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE3D, d1.Texture3D = {0, 0, t->depthOrArray};
        else if (t->samples > 1)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D10_RTV_DIMENSION_TEXTURE2DMS,
            d1.Texture2DMSArray = {0, t->depthOrArray};
        else
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_RTV_DIMENSION_TEXTURE2DARRAY : D3D10_RTV_DIMENSION_TEXTURE2D, d1.Texture2DArray = {0, 0, t->depthOrArray};
    }
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = d1.Format;
    d.ViewDimension = static_cast<D3D12_RTV_DIMENSION>(d1.ViewDimension);
    switch (d1.ViewDimension)
    {
    case D3D10_RTV_DIMENSION_TEXTURE1D:
        d.Texture1D.MipSlice = d1.Texture1D.MipSlice;
        break;
    case D3D10_RTV_DIMENSION_TEXTURE1DARRAY:
        d.Texture1DArray = {d1.Texture1DArray.MipSlice, d1.Texture1DArray.FirstArraySlice, d1.Texture1DArray.ArraySize};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2D:
        d.Texture2D = {d1.Texture2D.MipSlice, 0};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2DARRAY:
        d.Texture2DArray = {d1.Texture2DArray.MipSlice, d1.Texture2DArray.FirstArraySlice, d1.Texture2DArray.ArraySize, 0};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY:
        d.Texture2DMSArray = {d1.Texture2DMSArray.FirstArraySlice, d1.Texture2DMSArray.ArraySize};
        break;
    case D3D10_RTV_DIMENSION_TEXTURE3D:
        d.Texture3D = {d1.Texture3D.MipSlice, d1.Texture3D.FirstWSlice, d1.Texture3D.WSize};
        break;
    default:
        return E_INVALIDARG;
    }
    auto* v = new Rtv12;
    r->AddRef();
    v->resource = r;
    v->info = t;
    v->desc = d1;
    v->format = d1.Format != DXGI_FORMAT_UNKNOWN ? d1.Format : t->format;
    v->samples = t->samples;
    v->quality = t->quality;
    v->heap = &g_rtvCpu;
    v->cpu = g_rtvCpu.Alloc();
    g.dev->CreateRenderTargetView(t->res, &d, v->cpu);
    *out = v;
    return S_OK;
}

HRESULT Device::CreateDepthStencilView(ID3D10Resource* r, const D3D10_DEPTH_STENCIL_VIEW_DESC* desc, ID3D10DepthStencilView** out)
{
    TextureCore* t = r ? AsTexture(r) : nullptr;
    if (!t)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    D3D10_DEPTH_STENCIL_VIEW_DESC d1{};
    if (desc)
        d1 = *desc;
    else
    {
        d1.Format = t->format;
        if (t->dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_DSV_DIMENSION_TEXTURE1DARRAY : D3D10_DSV_DIMENSION_TEXTURE1D, d1.Texture1DArray = {0, 0, t->depthOrArray};
        else if (t->samples > 1)
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D10_DSV_DIMENSION_TEXTURE2DMS,
            d1.Texture2DMSArray = {0, t->depthOrArray};
        else
            d1.ViewDimension = t->depthOrArray > 1 ? D3D10_DSV_DIMENSION_TEXTURE2DARRAY : D3D10_DSV_DIMENSION_TEXTURE2D, d1.Texture2DArray = {0, 0, t->depthOrArray};
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = d1.Format;
    d.ViewDimension = static_cast<D3D12_DSV_DIMENSION>(d1.ViewDimension);
    switch (d1.ViewDimension)
    {
    case D3D10_DSV_DIMENSION_TEXTURE1D:
        d.Texture1D.MipSlice = d1.Texture1D.MipSlice;
        break;
    case D3D10_DSV_DIMENSION_TEXTURE1DARRAY:
        d.Texture1DArray = {d1.Texture1DArray.MipSlice, d1.Texture1DArray.FirstArraySlice, d1.Texture1DArray.ArraySize};
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2D:
        d.Texture2D.MipSlice = d1.Texture2D.MipSlice;
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2DARRAY:
        d.Texture2DArray = {d1.Texture2DArray.MipSlice, d1.Texture2DArray.FirstArraySlice, d1.Texture2DArray.ArraySize};
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY:
        d.Texture2DMSArray = {d1.Texture2DMSArray.FirstArraySlice, d1.Texture2DMSArray.ArraySize};
        break;
    default:
        return E_INVALIDARG;
    }
    auto* v = new Dsv12;
    r->AddRef();
    v->resource = r;
    v->info = t;
    v->desc = d1;
    v->format = d1.Format != DXGI_FORMAT_UNKNOWN ? d1.Format : t->format;
    v->samples = t->samples;
    v->quality = t->quality;
    v->heap = &g_dsvCpu;
    v->cpu = g_dsvCpu.Alloc();
    g.dev->CreateDepthStencilView(t->res, &d, v->cpu);
    *out = v;
    return S_OK;
}

HRESULT Device::CreateInputLayout(const D3D10_INPUT_ELEMENT_DESC* elements, UINT n, const void*, SIZE_T, ID3D10InputLayout** out)
{
    if (!elements && n)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    auto* l = new Layout12;
    for (UINT i = 0; i < n; i++)
    {
        const D3D10_INPUT_ELEMENT_DESC& e = elements[i];
        l->names.emplace_back(e.SemanticName ? e.SemanticName : "");
        l->elements.push_back({l->names.back().c_str(), e.SemanticIndex, e.Format, e.InputSlot, e.AlignedByteOffset,
                               static_cast<D3D12_INPUT_CLASSIFICATION>(e.InputSlotClass), e.InstanceDataStepRate});
        l->slotEnd = std::max(l->slotEnd, std::min(e.InputSlot + 1, 16u));
        l->animated = l->animated || e.InputSlotClass == D3D10_INPUT_PER_INSTANCE_DATA || _strnicmp(l->names.back().c_str(), "BLEND", 5) == 0;
        l->contentHash = HashBytes(l->names.back().data(), l->names.back().size(), l->contentHash ? l->contentHash : 1469598103934665603ull);
        const UINT fields[6] = {e.SemanticIndex, static_cast<UINT>(e.Format), e.InputSlot, e.AlignedByteOffset, static_cast<UINT>(e.InputSlotClass),
                                e.InstanceDataStepRate};
        l->contentHash = HashBytes(fields, sizeof(fields), l->contentHash);
    }
    *out = l;
    return S_OK;
}

template <class T, class Init>
HRESULT CreateState(StateCache<T>& cache, const void* desc, size_t bytes, T** out, Init init)
{
    std::string key(static_cast<const char*>(desc), bytes);
    std::lock_guard<std::mutex> guard(g_stateLock);
    if (T* found = cache.Find(key))
    {
        *out = found;
        return S_OK;
    }
    auto* s = new T;
    s->key = key;
    s->cache = &cache;
    init(s);
    cache.map[key] = s;
    *out = s;
    return S_OK;
}

HRESULT Device::CreateBlendState1(const D3D10_BLEND_DESC1* desc, ID3D10BlendState1** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Blend12* b = nullptr;
    CreateState(g_blendCache, desc, sizeof(*desc), &b, [desc](Blend12* s) {
        s->desc = *desc;
        s->d12 = Blend12From(*desc);
    });
    *out = b;
    return S_OK;
}

HRESULT Device::CreateDepthStencilState(const D3D10_DEPTH_STENCIL_DESC* desc, ID3D10DepthStencilState** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Depth12* d = nullptr;
    CreateState(g_depthCache, desc, sizeof(*desc), &d, [desc](Depth12* s) {
        s->desc = *desc;
        s->d12 = Depth12From(*desc);
    });
    *out = d;
    return S_OK;
}

HRESULT Device::CreateRasterizerState(const D3D10_RASTERIZER_DESC* desc, ID3D10RasterizerState** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Raster12* r = nullptr;
    CreateState(g_rasterCache, desc, sizeof(*desc), &r, [desc](Raster12* s) {
        s->desc = *desc;
        s->d12 = Raster12From(*desc);
    });
    *out = r;
    return S_OK;
}

HRESULT Device::CreateSamplerState(const D3D10_SAMPLER_DESC* desc, ID3D10SamplerState** out)
{
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    Sampler12* smp = nullptr;
    CreateState(g_samplerCache, desc, sizeof(*desc), &smp, [desc](Sampler12* s) {
        s->desc = *desc;
        D3D12_SAMPLER_DESC d{};
        d.Filter = static_cast<D3D12_FILTER>(desc->Filter & 0x7FFFFFFF);
        d.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc->AddressU);
        d.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc->AddressV);
        d.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc->AddressW);
        // CRYMT_MIPBIAS (tests): added to every sampler's mip bias (DLSS's guide: a bias against shimmering patterns or
        // for detail).
        static const float extraBias = [] {
            char text[32] = {};
            return GetEnvironmentVariableA("CRYMT_MIPBIAS", text, sizeof(text)) ? static_cast<float>(atof(text)) : 0.0f;
        }();
        d.MipLODBias = desc->MipLODBias + (desc->ComparisonFunc ? 0.0f : extraBias);
        d.MaxAnisotropy = std::min(16u, std::max(1u, desc->MaxAnisotropy));
        d.ComparisonFunc = desc->ComparisonFunc ? static_cast<D3D12_COMPARISON_FUNC>(desc->ComparisonFunc) : D3D12_COMPARISON_FUNC_NEVER;
        memcpy(d.BorderColor, desc->BorderColor, sizeof(d.BorderColor));
        d.MinLOD = desc->MinLOD;
        d.MaxLOD = desc->MaxLOD;
        s->cpu = g_samplerCpu.Alloc();
        g.dev->CreateSampler(&d, s->cpu);
    });
    *out = smp;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queries: occlusion counts and timestamps resolve into a readback buffer when they end; events are the end of a list.
// A query must begin and end in one command list, so active occlusion queries end and restart around submissions.
// ---------------------------------------------------------------------------------------------------------------------
std::vector<Query12*> g_activeQueries;

