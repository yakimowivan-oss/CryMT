// CryMT D3D12 back end: camera maths, buffers, textures, views and shaders (Buffer12, TextureCore, View12, Shader12).
// Included in D3D12Backend.cpp, where this text stood.
// a * b for D3DX-style 4x4 matrices (row vectors, rows in memory).
void MatMul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

// This view's points (row vectors: x, y, -distance, 1) to the previous frame's clip space, in double, from the two
// views' rotations and the camera's two places (2026-11-13). The float product RigidInverse(view) x previous view lost
// ~0.3 mm of the camera's place 2 km from the world's origin (the view's translation ~3000 has a float step of 0.24 mm,
// and the product cancels it): on a surface 1.5 m away that is 0.2-0.3 px at 5120x1440, the same every frame on a still
// camera (a false zoom) and noise while moving. DLSS kept sliding its history under the textures of near ground and walls
// ("textures flow after the camera moves"; edges, clamped by DLSS, stayed). The place comes from the CCamera (camPos, w
// 1); without it, from the view's translation in double. world = v R^T + c; previous view = world P + t' = v (R^T P) +
// (c - c') P.
void CameraPlace(const float* view, const float* camPos, double* c)
{
    if (camPos && camPos[3] == 1.0f)
    {
        for (int i = 0; i < 3; i++)
            c[i] = camPos[i];
        return;
    }
    for (int i = 0; i < 3; i++)
        c[i] = -(static_cast<double>(view[12]) * view[i * 4] + static_cast<double>(view[13]) * view[i * 4 + 1] + static_cast<double>(view[14]) * view[i * 4 + 2]);
}

void ReprojDouble(const float* view, const float* camPos, const float* prevView, const float* prevCamPos, const float* prevProj, float* out)
{
    double c[3], cp[3];
    CameraPlace(view, camPos, c);
    CameraPlace(prevView, prevCamPos, cp);
    double m[16] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
        {
            double s = 0.0;
            for (int k = 0; k < 3; k++)
                s += static_cast<double>(view[k * 4 + i]) * prevView[k * 4 + j];
            m[i * 4 + j] = s;
        }
    for (int j = 0; j < 3; j++)
    {
        double s = 0.0;
        for (int k = 0; k < 3; k++)
            s += (c[k] - cp[k]) * prevView[k * 4 + j];
        m[12 + j] = s;
    }
    m[15] = 1.0;
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
        {
            double s = 0.0;
            for (int k = 0; k < 4; k++)
                s += m[r * 4 + k] * prevProj[k * 4 + col];
            out[r * 4 + col] = static_cast<float>(s);
        }
}

// The cosine of the camera's turn between two views (their rotations' Frobenius product: trace(R^T R') = 1 + 2 cos).
double ViewTurnCos(const float* view, const float* prevView)
{
    double t = 0.0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            t += static_cast<double>(view[r * 4 + c]) * prevView[r * 4 + c];
    return (t - 1.0) / 2.0;
}

// The camera's move between two frames (metres), for the cut test.
double CameraJump(const float* view, const float* camPos, const float* prevView, const float* prevCamPos)
{
    double c[3], cp[3];
    CameraPlace(view, camPos, c);
    CameraPlace(prevView, prevCamPos, cp);
    return sqrt((c[0] - cp[0]) * (c[0] - cp[0]) + (c[1] - cp[1]) * (c[1] - cp[1]) + (c[2] - cp[2]) * (c[2] - cp[2]));
}

// Inverse of a rotation and translation (the view matrix): transposed rotation, translation moved back through it.
void RigidInverse(const float* m, float* out)
{
    for (int r = 0; r < 3; r++)
    {
        for (int c = 0; c < 3; c++)
            out[r * 4 + c] = m[c * 4 + r];
        out[r * 4 + 3] = 0.0f;
    }
    for (int c = 0; c < 3; c++)
        out[12 + c] = -(m[12] * m[c * 4] + m[13] * m[c * 4 + 1] + m[14] * m[c * 4 + 2]);
    out[15] = 1.0f;
}

// Waits until the GPU is done with a staging resource before the CPU touches it.
HRESULT WaitStaging(ResourceInfo* info, bool read, UINT flags)
{
    const uint64_t serial = read ? info->gpuWriteSerial : std::max(info->gpuWriteSerial, info->gpuReadSerial);
    if (!serial)
        return S_OK;
    PollDone();
    if (SerialDone(serial))
        return S_OK;
    if (serial >= g_mainSerial)
        SubmitMain();
    if (flags & D3D10_MAP_FLAG_DO_NOT_WAIT)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    WaitSerial(serial);
    return S_OK;
}

UINT FormatBytes(DXGI_FORMAT f)
{
    const int v = f;
    if (v >= 1 && v <= 4)
        return 16;
    if (v >= 5 && v <= 8)
        return 12;
    if (v >= 9 && v <= 22)
        return 8;
    if ((v >= 23 && v <= 47) || (v >= 67 && v <= 69) || (v >= 87 && v <= 93))
        return 4;
    if ((v >= 48 && v <= 59) || v == 85 || v == 86)
        return 2;
    if (v >= 60 && v <= 65)
        return 1;
    return 4;
}

class Buffer12 final : public Child<ID3D10Buffer>, public ResourceInfo
{
public:
    D3D10_BUFFER_DESC desc{};
    Block block{}; // default and immutable
    Slice slice{}; // dynamic: the current contents, in stream order (what draws bind)
    Slice prod{};  // dynamic: the producer's current memory (Dx12DiscardNow); equals `slice` once the stream caught up
    struct Retired
    {
        Slice slice;
        uint64_t serial;
    };
    std::deque<Retired> retired;
    uint32_t generation = 0; // dynamic renames, for buffer views
    // Default constant buffers: their first 12 floats as last written (creation, UpdateSubresource), in stream order
    // like the draws - the object matrix (PI_ObjMatrix at c0) the GI geometry's cache keys on.
    float head[12] = {};
    bool headKnown = false;

    Buffer12()
    {
        buffer = true;
    }

    bool Dynamic() const
    {
        return desc.Usage == D3D10_USAGE_DYNAMIC;
    }

    bool Staging() const
    {
        return desc.Usage == D3D10_USAGE_STAGING;
    }

    ID3D12Resource* Res() const
    {
        return Dynamic() ? slice.res : Staging() ? res : block.page ? block.page->res : nullptr;
    }

    uint64_t Offset() const
    {
        return Dynamic() ? slice.offset : Staging() ? 0 : block.offset;
    }

    D3D12_GPU_VIRTUAL_ADDRESS Gpu() const
    {
        return Dynamic() ? slice.gpu : Staging() ? res->GetGPUVirtualAddress() : block.gpu;
    }

    // Default buffers live in a page whose state matters within the main list; null for the others.
    BufferPage* Page() const
    {
        return !Dynamic() && !Staging() ? block.page : nullptr;
    }

    void STDMETHODCALLTYPE GetType(D3D10_RESOURCE_DIMENSION* d) override
    {
        *d = D3D10_RESOURCE_DIMENSION_BUFFER;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}

    UINT STDMETHODCALLTYPE GetEvictionPriority() override
    {
        return 0;
    }

    HRESULT STDMETHODCALLTYPE Map(D3D10_MAP type, UINT flags, void** data) override
    {
        if (!data)
            return E_INVALIDARG;
        if (Dynamic())
        {
            if (type == D3D10_MAP_WRITE_DISCARD)
                Rename();
            else if (type != D3D10_MAP_WRITE_NO_OVERWRITE)
                return E_INVALIDARG;
            *data = slice.cpu;
            return slice.cpu ? S_OK : E_OUTOFMEMORY;
        }
        if (Staging())
        {
            const HRESULT hr = WaitStaging(this, type == D3D10_MAP_READ || type == D3D10_MAP_READ_WRITE, flags);
            if (FAILED(hr))
                return hr;
            *data = stagingCpu;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    void STDMETHODCALLTYPE Unmap() override {}

    void STDMETHODCALLTYPE GetDesc(D3D10_BUFFER_DESC* d) override
    {
        *d = desc;
    }

    // Map(DISCARD): the current slice goes back once the list being recorded has completed.
    void Rename()
    {
        const int kind = (desc.BindFlags & D3D10_BIND_CONSTANT_BUFFER) ? 0 : (desc.BindFlags & (D3D10_BIND_VERTEX_BUFFER | D3D10_BIND_INDEX_BUFFER)) ? 1 : 2;
        g_renames[kind]++;
        g_renameBytes[kind] += desc.ByteWidth;
        retired.push_back({slice, g_mainSerial});
        if (SerialDone(retired.front().serial + SliceMargin()))
        {
            slice = retired.front().slice;
            retired.pop_front();
        }
        else
            slice = g_slices.Alloc(desc.ByteWidth);
        generation++;
    }

    // Stream thread: the memory the producer took at Map(DISCARD) becomes the contents; the old one goes back to the
    // heap once the list being recorded has completed (retired slices the GPU is done with go back now).
    void ApplySlice(const Slice& s)
    {
        const int kind = (desc.BindFlags & D3D10_BIND_CONSTANT_BUFFER) ? 0 : (desc.BindFlags & (D3D10_BIND_VERTEX_BUFFER | D3D10_BIND_INDEX_BUFFER)) ? 1 : 2;
        g_renames[kind]++;
        g_renameBytes[kind] += desc.ByteWidth;
        retired.push_back({slice, g_mainSerial});
        slice = s;
        generation++;
        while (!retired.empty() && SerialDone(retired.front().serial + SliceMargin()))
        {
            g_slices.Free(retired.front().slice);
            retired.pop_front();
        }
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10Resource)) || Child::Supports(riid);
    }

    void Destroy() override
    {
        if (Dynamic())
        {
            std::vector<Slice> slices{slice};
            for (const Retired& r : retired)
                slices.push_back(r.slice);
            // Memory the producer took that the stream never applied is in neither list (without the producer path
            // prod stays the creation slice, which is in one of them).
            bool known = prod.cpu == slice.cpu;
            for (const Retired& r : retired)
                known = known || r.slice.cpu == prod.cpu;
            if (!known)
                slices.push_back(prod);
            DeferCall([slices] {
                for (const Slice& s : slices)
                    g_slices.Free(s);
            });
        }
        else if (Staging())
            DeferRelease(res);
        else
        {
            const Block b = block;
            DeferCall([b] { g_blocks.Free(b); });
        }
        Child::Destroy();
    }
};

// Direct dynamic-buffer maps (see D3D12Backend.h). The buffer's vtable tells ours from others (the stream patches its
// Map/Unmap entries in place, the pointer stays).
const void* g_buffer12Vtable;
uint64_t g_directDiscards, g_directDiscardBytes; // producer side

Buffer12* OurDynamicBuffer(ID3D10Buffer* buffer)
{
    if (!buffer || !g_buffer12Vtable || *reinterpret_cast<void* const*>(buffer) != g_buffer12Vtable)
        return nullptr;
    auto* b = static_cast<Buffer12*>(buffer);
    return b->Dynamic() ? b : nullptr;
}

bool DiscardNow(ID3D10Buffer* buffer, Dx12Discard* out)
{
    Buffer12* b = OurDynamicBuffer(buffer);
    if (!b)
        return false;
    const Slice s = g_slices.Alloc(b->desc.ByteWidth);
    out->cpu = s.cpu;
    if (!s.cpu)
        return true;
    b->prod = s;
    out->words[0] = reinterpret_cast<uint64_t>(s.res);
    out->words[1] = s.offset;
    out->words[2] = reinterpret_cast<uint64_t>(s.cpu);
    out->words[3] = s.gpu;
    out->words[4] = static_cast<uint64_t>(static_cast<int64_t>(s.cls));
    g_directDiscards++;
    g_directDiscardBytes += b->desc.ByteWidth;
    return true;
}

void* ProducerMemory(ID3D10Buffer* buffer)
{
    Buffer12* b = OurDynamicBuffer(buffer);
    return b ? b->prod.cpu : nullptr;
}

void ApplyDiscard(ID3D10Buffer* buffer, const uint64_t* w)
{
    Slice s;
    s.res = reinterpret_cast<ID3D12Resource*>(w[0]);
    s.offset = w[1];
    s.cpu = reinterpret_cast<uint8_t*>(w[2]);
    s.gpu = w[3];
    s.cls = static_cast<int>(static_cast<int64_t>(w[4]));
    static_cast<Buffer12*>(buffer)->ApplySlice(s);
}

void DirectMapStats(uint64_t* discards, uint64_t* bytes)
{
    *discards = g_directDiscards;
    *bytes = g_directDiscardBytes;
}

// The format a depth buffer's views use, for a depth resource described typed or typeless.
DXGI_FORMAT DepthViewFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
        return f;
    }
}

bool TypelessColor(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return true;
    default:
        return false;
    }
}

// Textures of all dimensions share this; the interfaces differ only in Map's signature and the description type.
struct TextureCore : ResourceInfo
{
    D3D10_RESOURCE_DIMENSION dim{};
    D3D10_USAGE usage{};
    UINT bind = 0, cpuAccess = 0, misc = 0;
    DXGI_FORMAT format{};
    UINT width = 1, height = 1, depthOrArray = 1, mips = 1, samples = 1, quality = 0;
    UINT subresources = 1;
    // Staging: a buffer holding every subresource at its copyable footprint.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    // Dynamic: Map(DISCARD) writes to upload memory, Unmap records the copy.
    struct Pending
    {
        UploadAlloc up;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        bool active;
    };
    std::vector<Pending> pending;

    bool Staging() const
    {
        return usage == D3D10_USAGE_STAGING;
    }

    D3D12_RESOURCE_DESC Desc12() const
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = dim == D3D10_RESOURCE_DIMENSION_TEXTURE1D   ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                      : dim == D3D10_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                  : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = static_cast<UINT16>(depthOrArray);
        d.MipLevels = static_cast<UINT16>(mips);
        d.Format = format;
        d.SampleDesc = {samples, quality};
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (bind & D3D10_BIND_RENDER_TARGET)
            d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (bind & D3D10_BIND_DEPTH_STENCIL)
        {
            // Depth buffers are typeless so they can also be read (DLSS reads the scene depth); views keep the
            // renderer's formats. (Measured: typeless depth costs no GPU time.)
            d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            switch (format)
            {
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
                d.Format = DXGI_FORMAT_R24G8_TYPELESS;
                break;
            case DXGI_FORMAT_D32_FLOAT:
                d.Format = DXGI_FORMAT_R32_TYPELESS;
                break;
            case DXGI_FORMAT_D16_UNORM:
                d.Format = DXGI_FORMAT_R16_TYPELESS;
                break;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
                break;
            default:
                break;
            }
        }
        return d;
    }

    HRESULT MapSub(UINT sub, D3D10_MAP type, UINT flags, void** data, UINT* rowPitch, UINT* depthPitch);
    void UnmapSub(UINT sub);

    void FreeMemory()
    {
        DeferRelease(res);
        res = nullptr;
    }
};

template <class I, class Desc, D3D10_RESOURCE_DIMENSION Dim>
class Texture12 : public Child<I>, public TextureCore
{
public:
    Desc desc{};

    void STDMETHODCALLTYPE GetType(D3D10_RESOURCE_DIMENSION* d) override
    {
        *d = Dim;
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}

    UINT STDMETHODCALLTYPE GetEvictionPriority() override
    {
        return 0;
    }

    void STDMETHODCALLTYPE GetDesc(Desc* d) override
    {
        *d = desc;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10Resource)) || Child<I>::Supports(riid);
    }

    void Destroy() override
    {
        FreeMemory();
        Child<I>::Destroy();
    }
};

class Texture1D12 final : public Texture12<ID3D10Texture1D, D3D10_TEXTURE1D_DESC, D3D10_RESOURCE_DIMENSION_TEXTURE1D>
{
public:
    HRESULT STDMETHODCALLTYPE Map(UINT sub, D3D10_MAP type, UINT flags, void** data) override
    {
        return MapSub(sub, type, flags, data, nullptr, nullptr);
    }

    void STDMETHODCALLTYPE Unmap(UINT sub) override
    {
        UnmapSub(sub);
    }
};

class Texture2D12 final : public Texture12<ID3D10Texture2D, D3D10_TEXTURE2D_DESC, D3D10_RESOURCE_DIMENSION_TEXTURE2D>
{
public:
    HRESULT STDMETHODCALLTYPE Map(UINT sub, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE2D* m) override
    {
        if (!m)
            return E_INVALIDARG;
        return MapSub(sub, type, flags, &m->pData, &m->RowPitch, nullptr);
    }

    void STDMETHODCALLTYPE Unmap(UINT sub) override
    {
        UnmapSub(sub);
    }
};

class Texture3D12 final : public Texture12<ID3D10Texture3D, D3D10_TEXTURE3D_DESC, D3D10_RESOURCE_DIMENSION_TEXTURE3D>
{
public:
    HRESULT STDMETHODCALLTYPE Map(UINT sub, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE3D* m) override
    {
        if (!m)
            return E_INVALIDARG;
        return MapSub(sub, type, flags, &m->pData, &m->RowPitch, &m->DepthPitch);
    }

    void STDMETHODCALLTYPE Unmap(UINT sub) override
    {
        UnmapSub(sub);
    }
};

ResourceInfo* Info(ID3D10Resource* r)
{
    if (!r)
        return nullptr;
    D3D10_RESOURCE_DIMENSION d;
    r->GetType(&d);
    switch (d)
    {
    case D3D10_RESOURCE_DIMENSION_BUFFER:
        return static_cast<Buffer12*>(static_cast<ID3D10Buffer*>(r));
    case D3D10_RESOURCE_DIMENSION_TEXTURE1D:
        return static_cast<Texture1D12*>(static_cast<ID3D10Texture1D*>(r));
    case D3D10_RESOURCE_DIMENSION_TEXTURE2D:
        return static_cast<Texture2D12*>(static_cast<ID3D10Texture2D*>(r));
    case D3D10_RESOURCE_DIMENSION_TEXTURE3D:
        return static_cast<Texture3D12*>(static_cast<ID3D10Texture3D*>(r));
    default:
        return nullptr;
    }
}

inline Buffer12* AsBuffer(ID3D10Resource* r)
{
    D3D10_RESOURCE_DIMENSION d;
    r->GetType(&d);
    return d == D3D10_RESOURCE_DIMENSION_BUFFER ? static_cast<Buffer12*>(static_cast<ID3D10Buffer*>(r)) : nullptr;
}

inline TextureCore* AsTexture(ID3D10Resource* r)
{
    ResourceInfo* info = Info(r);
    return info && !info->buffer ? static_cast<TextureCore*>(info) : nullptr;
}

// Views keep their resource alive and own a CPU descriptor.
template <class I>
class View12 : public Child<I>
{
public:
    ID3D10Resource* resource = nullptr;
    ResourceInfo* info = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    CpuDescriptors* heap = nullptr;

    void STDMETHODCALLTYPE GetResource(ID3D10Resource** out) override
    {
        resource->AddRef();
        *out = resource;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10View)) || Child<I>::Supports(riid);
    }

    void Destroy() override
    {
        heap->Free(cpu);
        resource->Release();
        Child<I>::Destroy();
    }
};

class Srv12 final : public View12<ID3D10ShaderResourceView1>
{
public:
    D3D10_SHADER_RESOURCE_VIEW_DESC1 desc{};
    D3D12_SHADER_RESOURCE_VIEW_DESC d12{};
    uint32_t generation = 0; // dynamic buffer views: the rename the descriptor was written for

    void STDMETHODCALLTYPE GetDesc(D3D10_SHADER_RESOURCE_VIEW_DESC* d) override
    {
        memcpy(d, &desc, sizeof(*d));
    }

    void STDMETHODCALLTYPE GetDesc1(D3D10_SHADER_RESOURCE_VIEW_DESC1* d) override
    {
        *d = desc;
    }

    // Buffer views of dynamic buffers follow the buffer's renames.
    void Refresh()
    {
        if (!info->buffer)
            return;
        auto* b = static_cast<Buffer12*>(info);
        if (!b->Dynamic() || generation == b->generation)
            return;
        generation = b->generation;
        D3D12_SHADER_RESOURCE_VIEW_DESC d = d12;
        d.Buffer.FirstElement += b->Offset() / FormatBytes(d.Format);
        g.dev->CreateShaderResourceView(b->Res(), &d, cpu);
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10ShaderResourceView)) || View12::Supports(riid);
    }
};

class Rtv12 final : public View12<ID3D10RenderTargetView>
{
public:
    D3D10_RENDER_TARGET_VIEW_DESC desc{};
    DXGI_FORMAT format{};
    UINT samples = 1, quality = 0;

    void STDMETHODCALLTYPE GetDesc(D3D10_RENDER_TARGET_VIEW_DESC* d) override
    {
        *d = desc;
    }
};

class Dsv12 final : public View12<ID3D10DepthStencilView>
{
public:
    D3D10_DEPTH_STENCIL_VIEW_DESC desc{};
    DXGI_FORMAT format{};
    UINT samples = 1, quality = 0;

    void STDMETHODCALLTYPE GetDesc(D3D10_DEPTH_STENCIL_VIEW_DESC* d) override
    {
        *d = desc;
    }
};

// State objects are shared by description, as in D3D10 (the same object is returned for the same description).
std::mutex g_stateLock;

template <class T>
struct StateCache
{
    std::unordered_map<std::string, T*> map;

    T* Find(const std::string& key) // under g_stateLock; returns a new reference
    {
        const auto it = map.find(key);
        return it != map.end() && it->second->TryAddRef() ? it->second : nullptr;
    }

    void Erase(const std::string& key, T* object) // under g_stateLock
    {
        const auto it = map.find(key);
        if (it != map.end() && it->second == object)
            map.erase(it);
    }
};

template <class I, class Self>
class State12 : public Child<I>
{
public:
    std::string key;
    StateCache<Self>* cache = nullptr;

protected:
    void Destroy() override
    {
        {
            std::lock_guard<std::mutex> guard(g_stateLock);
            cache->Erase(key, static_cast<Self*>(this));
        }
        Child<I>::Destroy();
    }
};

class Blend12 final : public State12<ID3D10BlendState1, Blend12>
{
public:
    D3D10_BLEND_DESC1 desc{};
    D3D12_BLEND_DESC d12{};

    void STDMETHODCALLTYPE GetDesc(D3D10_BLEND_DESC* d) override
    {
        d->AlphaToCoverageEnable = desc.AlphaToCoverageEnable;
        const D3D10_RENDER_TARGET_BLEND_DESC1& rt = desc.RenderTarget[0];
        d->SrcBlend = rt.SrcBlend;
        d->DestBlend = rt.DestBlend;
        d->BlendOp = rt.BlendOp;
        d->SrcBlendAlpha = rt.SrcBlendAlpha;
        d->DestBlendAlpha = rt.DestBlendAlpha;
        d->BlendOpAlpha = rt.BlendOpAlpha;
        for (int i = 0; i < 8; i++)
        {
            d->BlendEnable[i] = desc.RenderTarget[i].BlendEnable;
            d->RenderTargetWriteMask[i] = desc.RenderTarget[i].RenderTargetWriteMask;
        }
    }

    void STDMETHODCALLTYPE GetDesc1(D3D10_BLEND_DESC1* d) override
    {
        *d = desc;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10BlendState)) || State12::Supports(riid);
    }
};

class Depth12 final : public State12<ID3D10DepthStencilState, Depth12>
{
public:
    D3D10_DEPTH_STENCIL_DESC desc{};
    D3D12_DEPTH_STENCIL_DESC d12{};

    void STDMETHODCALLTYPE GetDesc(D3D10_DEPTH_STENCIL_DESC* d) override
    {
        *d = desc;
    }
};

class Raster12 final : public State12<ID3D10RasterizerState, Raster12>
{
public:
    D3D10_RASTERIZER_DESC desc{};
    D3D12_RASTERIZER_DESC d12{};

    void STDMETHODCALLTYPE GetDesc(D3D10_RASTERIZER_DESC* d) override
    {
        *d = desc;
    }
};

class Sampler12 final : public State12<ID3D10SamplerState, Sampler12>
{
public:
    D3D10_SAMPLER_DESC desc{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};

    void STDMETHODCALLTYPE GetDesc(D3D10_SAMPLER_DESC* d) override
    {
        *d = desc;
    }

protected:
    void Destroy() override
    {
        g_samplerCpu.Free(cpu);
        State12::Destroy();
    }
};

StateCache<Blend12> g_blendCache;
StateCache<Depth12> g_depthCache;
StateCache<Raster12> g_rasterCache;
StateCache<Sampler12> g_samplerCache;

class Layout12 final : public Child<ID3D10InputLayout>
{
public:
    uint64_t contentHash = 0;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::deque<std::string> names; // stable storage for the semantic names
    UINT slotEnd = 0;              // highest input slot + 1
    bool animated = false;         // bone weights or indices (skinned), or a per-instance stream: the GI geometry
                                   // voxelizes its draws every time (RunVoxelGeometry)
};

// A fast 64-bit hash, 8 bytes a step (GI geometry keys: ~2300 draws a frame; byte-wise FNV and the CRT's lrintf
// there cost ~1.3 ms a frame on the stream thread, the sampler showed).
uint64_t HashWords(const void* data, size_t bytes, uint64_t h)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (; bytes >= 8; bytes -= 8, p += 8)
    {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; bytes; bytes--, p++)
        h = (h ^ *p) * 0x100000001B3ull;
    return h;
}

// Shaders keep their bytecode for pipeline state creation and what reflection says they bind.
uint64_t HashBytes(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

template <class I>
class Shader12 final : public Child<I>
{
public:
    uint64_t contentHash = 0; // for pipeline library names, stable across runs
    std::vector<uint8_t> code;
    uint32_t cbMask = 0; // constant buffer slots
    UINT srvEnd = 0;     // highest texture slot + 1
    UINT samplerEnd = 0; // highest sampler slot + 1
    bool motionBlur = false; // the object motion blur pass (its previous object matrix, MotionBlurData, in the reflection data)
    // For the GI geometry pass: a vertex shader's outputs (a pixel shader reading its texture coordinates must declare
    // them alike), a pixel shader's lowest 2D texture slot and whether it discards (alpha test).
    struct Output
    {
        std::string name;
        UINT index, reg;
        BYTE mask;
        D3D10_REGISTER_COMPONENT_TYPE type;
        D3D10_NAME value;
    };
    std::vector<Output> outputs;
    int firstTexture = -1;
    int diffuseSlot = -1; // the texture diffuseMapSampler (the z-prepass's alpha test reads its alpha), -1 none
    bool refractive = false; // water, glass, ice, liquids (no CryMT reflection): see IsRefractiveResource
    bool noEnv = false;      // eyes, hair, skin (Eye.cfx, Hair.cfx, HumanSkin.cfx by their own parameters): no CryMT reflection
    int envCubeSlot = -1, env2DSlot = -1; // envMapSamplerCUBE ($EnvironmentCubeMap) / envMapSampler ($Env): the material's env map
    int waterEnvSamplerSlot = -1;         // Water.cfx's CryMT_WaterEnvSampler (its sampler state: the back end's trilinear clamp)
    bool water = false; // a vertex shader with the constant WaterLevel (WaterOcean.cfx, its z-prepass too): not voxel geometry
    bool discards = false;
    bool writesTarget1 = false; // a pixel shader writing SV_Target1 (the patched z-prepass: the foliage mask)
    bool writesTarget2 = false; // ... SV_Target2 (the patched z-prepass: wind-bent vegetation's screen move)
    bool particleLayer = false; // ... a one-component SV_Target2: the patched particles' layer (BindParticleLayer)
    bool plainCopy = false;     // a pixel shader that only samples one texture into its output (IsPlainCopy)
    // The engine's fog constants where the shader declares them (constant buffer slot, byte offset; -1 none): vfParams,
    // vfFogColor, slHazeColPartialMieInScatter, slHazeColPartialRayleighInScatter (RunVfog reads them in the fog pass).
    int fogSlot[4] = {-1, -1, -1, -1};
    UINT fogOffset[4] = {};
    int vfogSlot = -1;      // the patched fog pass's CryMT_VFogSampler texture slot (RunVfog's volume goes there)
    int vfogParamSlot = -1; // ... its CryMT_VFogParamSampler slot (the parameters, with the mark)

    // SM4 token stream (the SHDR chunk): opcode in bits 0-10, length in 24-30 (custom data: the next token).
    static bool HasDiscard(const std::vector<uint8_t>& dxbc)
    {
        if (dxbc.size() < 32 || memcmp(dxbc.data(), "DXBC", 4) != 0)
            return false;
        const uint32_t chunks = *reinterpret_cast<const uint32_t*>(&dxbc[28]);
        for (uint32_t c = 0; c < chunks && 32 + c * 4 + 4 <= dxbc.size(); c++)
        {
            const uint32_t at = *reinterpret_cast<const uint32_t*>(&dxbc[32 + c * 4]);
            if (at + 8 > dxbc.size() || (memcmp(&dxbc[at], "SHDR", 4) != 0 && memcmp(&dxbc[at], "SHEX", 4) != 0))
                continue;
            const uint32_t bytes = *reinterpret_cast<const uint32_t*>(&dxbc[at + 4]);
            const uint32_t* t = reinterpret_cast<const uint32_t*>(&dxbc[at + 8]);
            const size_t n = std::min<size_t>(bytes, dxbc.size() - at - 8) / 4;
            for (size_t i = 2; i < n;)
            {
                const uint32_t op = t[i] & 0x7FF;
                if (op == 13) // discard
                    return true;
                const uint32_t len = op == 0x35 && i + 1 < n ? t[i + 1] : (t[i] >> 24) & 0x7F;
                i += len ? len : 1;
            }
        }
        return false;
    }

    // A pixel shader that only copies a texture: declarations, one sample, ret (SM4 opcodes: 0x45 sample, 0x3E ret, 0x35
    // custom data, 0x58-0x6A declarations). The engine copies its jittered scene with one (12c069f424600b76) before DLSS.
    static bool IsPlainCopy(const std::vector<uint8_t>& dxbc)
    {
        if (dxbc.size() < 32 || memcmp(dxbc.data(), "DXBC", 4) != 0)
            return false;
        const uint32_t chunks = *reinterpret_cast<const uint32_t*>(&dxbc[28]);
        for (uint32_t c = 0; c < chunks && 32 + c * 4 + 4 <= dxbc.size(); c++)
        {
            const uint32_t at = *reinterpret_cast<const uint32_t*>(&dxbc[32 + c * 4]);
            if (at + 8 > dxbc.size() || (memcmp(&dxbc[at], "SHDR", 4) != 0 && memcmp(&dxbc[at], "SHEX", 4) != 0))
                continue;
            const uint32_t bytes = *reinterpret_cast<const uint32_t*>(&dxbc[at + 4]);
            const uint32_t* t = reinterpret_cast<const uint32_t*>(&dxbc[at + 8]);
            const size_t n = std::min<size_t>(bytes, dxbc.size() - at - 8) / 4;
            int samples = 0;
            for (size_t i = 2; i < n;)
            {
                const uint32_t op = t[i] & 0x7FF;
                if (op == 0x45)
                    samples++;
                else if (op != 0x3E && op != 0x35 && (op < 0x58 || op > 0x6A))
                    return false;
                const uint32_t len = op == 0x35 && i + 1 < n ? t[i + 1] : (t[i] >> 24) & 0x7F;
                i += len ? len : 1;
            }
            return samples == 1;
        }
        return false;
    }

    void Reflect()
    {
        discards = HasDiscard(code);
        plainCopy = IsPlainCopy(code);
        static const char kName[] = "MotionBlurData";
        motionBlur = std::search(code.begin(), code.end(), kName, kName + sizeof(kName) - 1) != code.end();
        ID3D10ShaderReflection* r = nullptr;
        if (!g.reflect || FAILED(g.reflect(code.data(), code.size(), &r)) || !r)
        {
            cbMask = 0xFFFF;
            srvEnd = kTableSrvs;
            samplerEnd = kTableSamplers;
            return;
        }
        D3D10_SHADER_DESC d{};
        r->GetDesc(&d);
        std::string resourceNames; // for the environment map log below
        for (UINT i = 0; i < d.BoundResources; i++)
        {
            D3D10_SHADER_INPUT_BIND_DESC b{};
            if (FAILED(r->GetResourceBindingDesc(i, &b)))
                continue;
            const UINT end = b.BindPoint + b.BindCount;
            if (b.Type == D3D10_SIT_TEXTURE && b.Dimension == D3D10_SRV_DIMENSION_TEXTURE2D &&
                (firstTexture < 0 || static_cast<int>(b.BindPoint) < firstTexture))
                firstTexture = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "diffuseMapSampler") == 0)
                diffuseSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && IsRefractiveResource(b.Name))
                refractive = true;
            if (b.Name && resourceNames.size() < 600)
                resourceNames += std::string(resourceNames.empty() ? "" : " ") + b.Name;
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "envMapSamplerCUBE") == 0)
                envCubeSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "envMapSampler") == 0)
                env2DSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_SAMPLER && b.Name && strcmp(b.Name, "CryMT_WaterEnvSampler") == 0)
                waterEnvSamplerSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "CryMT_VFogSampler") == 0)
                vfogSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_TEXTURE && b.Name && strcmp(b.Name, "CryMT_VFogParamSampler") == 0)
                vfogParamSlot = static_cast<int>(b.BindPoint);
            if (b.Type == D3D10_SIT_CBUFFER)
                cbMask |= ((1u << b.BindCount) - 1) << b.BindPoint;
            else if (b.Type == D3D10_SIT_SAMPLER)
                samplerEnd = std::max(samplerEnd, end);
            else
                srvEnd = std::max(srvEnd, end);
        }
        // Eyes, hair and skin keep their own look (the user, 2026-11-13): told by parameters only their shaders have.
        static const char* const kNoEnvParams[] = {"IrisColor", "CorneaOpacity", "IrisGlintMul", "HairColor", "PrimarySpecShift", "SecondarySpecShift",
                                                   "PrimarySpecColor", "Melanin", "SubsurfaceStrenght", "DiffusionAmount", "WrinklesBlend"};
        for (UINT i = 0; i < d.ConstantBuffers && !noEnv; i++)
            if (ID3D10ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i))
            {
                D3D10_SHADER_BUFFER_DESC bd{};
                if (SUCCEEDED(cb->GetDesc(&bd)))
                    for (UINT j = 0; j < bd.Variables && !noEnv; j++)
                    {
                        D3D10_SHADER_VARIABLE_DESC vd{};
                        ID3D10ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
                        if (var && SUCCEEDED(var->GetDesc(&vd)) && vd.Name)
                            for (const char* p : kNoEnvParams)
                                noEnv = noEnv || strcmp(vd.Name, p) == 0;
                    }
            }
        for (UINT i = 0; i < d.ConstantBuffers && !water; i++)
            if (ID3D10ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i))
            {
                D3D10_SHADER_BUFFER_DESC bd{};
                if (SUCCEEDED(cb->GetDesc(&bd)))
                    for (UINT j = 0; j < bd.Variables && !water; j++)
                    {
                        D3D10_SHADER_VARIABLE_DESC vd{};
                        ID3D10ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
                        water = var && SUCCEEDED(var->GetDesc(&vd)) && vd.Name && strcmp(vd.Name, "WaterLevel") == 0;
                    }
            }
        if (water)
        {
            static int logged;
            if (logged++ < 4)
                Log("ocean vertex shader recognized (WaterLevel), %u bytes", static_cast<unsigned>(code.size()));
        }
        if (vfogSlot >= 0 || vfogParamSlot >= 0)
        {
            static int logged;
            if (logged++ < 6)
                Log("fog pass shader with the GI in the fog: volume t%d, parameters t%d", vfogSlot, vfogParamSlot);
        }
        if (envCubeSlot >= 0 || env2DSlot >= 0)
        {
            static int kept, replaced;
            if (refractive ? kept++ < 24 : replaced++ < 8)
                Log("pixel shader with an environment map %s: %s", refractive ? "keeps it (water, glass)" : "may take CryMT's", resourceNames.c_str());
        }
        for (UINT i = 0; i < d.OutputParameters; i++)
        {
            D3D10_SIGNATURE_PARAMETER_DESC o{};
            if (SUCCEEDED(r->GetOutputParameterDesc(i, &o)) && o.SemanticName)
            {
                outputs.push_back({o.SemanticName, o.SemanticIndex, o.Register, o.Mask, o.ComponentType, o.SystemValueType});
                if (_stricmp(o.SemanticName, "SV_Target") == 0 && o.SemanticIndex == 1) // reflected with system value 0
                    writesTarget1 = true;
                if (_stricmp(o.SemanticName, "SV_Target") == 0 && o.SemanticIndex == 2)
                {
                    writesTarget2 = true;
                    particleLayer = o.Mask == 0x1;
                }
            }
        }
        // The fog's constants (fogSlot): the buffer's slot from its binding by name.
        static const char* const kFogVars[4] = {"vfParams", "vfFogColor", "slHazeColPartialMieInScatter", "slHazeColPartialRayleighInScatter"};
        for (UINT i = 0; i < d.ConstantBuffers; i++)
            if (ID3D10ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i))
            {
                D3D10_SHADER_BUFFER_DESC bd{};
                if (FAILED(cb->GetDesc(&bd)) || !bd.Name)
                    continue;
                int slot = -1;
                for (UINT b = 0; b < d.BoundResources && slot < 0; b++)
                {
                    D3D10_SHADER_INPUT_BIND_DESC bind{};
                    if (SUCCEEDED(r->GetResourceBindingDesc(b, &bind)) && bind.Type == D3D10_SIT_CBUFFER && bind.Name && strcmp(bind.Name, bd.Name) == 0)
                        slot = static_cast<int>(bind.BindPoint);
                }
                for (UINT j = 0; j < bd.Variables && slot >= 0; j++)
                {
                    D3D10_SHADER_VARIABLE_DESC vd{};
                    ID3D10ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
                    if (var && SUCCEEDED(var->GetDesc(&vd)) && vd.Name)
                        for (int k = 0; k < 4; k++)
                            if (strcmp(vd.Name, kFogVars[k]) == 0)
                            {
                                fogSlot[k] = slot;
                                fogOffset[k] = vd.StartOffset;
                            }
                }
            }
        r->Release();
    }
};
using VS12 = Shader12<ID3D10VertexShader>;
using GS12 = Shader12<ID3D10GeometryShader>;
using PS12 = Shader12<ID3D10PixelShader>;

class Query12 final : public Child<ID3D10Query>
{
public:
    D3D10_QUERY_DESC desc{};
    int heapIndex = -1;     // occlusion and timestamp queries
    uint64_t serial = 0;    // main list that ends it
    bool ended = false;
    bool split = false;     // an occlusion count that spans a submission: the first part is in the partial slot

    void STDMETHODCALLTYPE Begin() override;
    void STDMETHODCALLTYPE End() override;
    HRESULT STDMETHODCALLTYPE GetData(void* data, UINT size, UINT flags) override;

    UINT STDMETHODCALLTYPE GetDataSize() override
    {
        switch (desc.Query)
        {
        case D3D10_QUERY_EVENT:
        case D3D10_QUERY_OCCLUSION_PREDICATE:
            return sizeof(BOOL);
        case D3D10_QUERY_TIMESTAMP_DISJOINT:
            return sizeof(D3D10_QUERY_DATA_TIMESTAMP_DISJOINT);
        default:
            return sizeof(UINT64);
        }
    }

    void STDMETHODCALLTYPE GetDesc(D3D10_QUERY_DESC* d) override
    {
        *d = desc;
    }

protected:
    bool Supports(REFIID riid) const override
    {
        return IsEqualIID(riid, __uuidof(ID3D10Asynchronous)) || Child::Supports(riid);
    }

    void Destroy() override;
};
// ---------------------------------------------------------------------------------------------------------------------
// Creation uploads: their own command list, executed before the next main list (or alone when the ring runs out).
// ---------------------------------------------------------------------------------------------------------------------
AllocatorPool g_uploadAllocs;
ID3D12GraphicsCommandList* g_uploadList;
ID3D12CommandAllocator* g_uploadAlloc;
bool g_uploadOpen;
UploadRing g_initRing;
uint64_t g_uploadBytes, g_uploadSubmits;

ID3D12GraphicsCommandList* UploadList() // under g.lock
{
    if (!g_uploadOpen)
    {
        g_uploadAlloc = g_uploadAllocs.Get();
        if (!g_uploadList)
            g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_uploadAlloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                                     reinterpret_cast<void**>(&g_uploadList));
        else
            g_uploadList->Reset(g_uploadAlloc, nullptr);
        g_uploadOpen = true;
    }
    return g_uploadList;
}

