// CryMT D3D12 back end: Device: queries.
// Included in D3D12Backend.cpp, where this text stood.
bool OcclusionQuery(const Query12* q)
{
    return q->desc.Query == D3D10_QUERY_OCCLUSION || q->desc.Query == D3D10_QUERY_OCCLUSION_PREDICATE;
}

D3D12_QUERY_TYPE OcclusionType(const Query12* q)
{
    return q->desc.Query == D3D10_QUERY_OCCLUSION_PREDICATE ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION;
}

HRESULT Device::CreateQuery(const D3D10_QUERY_DESC* desc, ID3D10Query** out)
{
    if (!desc)
        return E_INVALIDARG;
    switch (desc->Query)
    {
    case D3D10_QUERY_EVENT:
    case D3D10_QUERY_OCCLUSION:
    case D3D10_QUERY_OCCLUSION_PREDICATE:
    case D3D10_QUERY_TIMESTAMP:
    case D3D10_QUERY_TIMESTAMP_DISJOINT:
        break;
    default:
        LogLimited("querytype", "query type %d is not supported", desc->Query);
        return E_NOTIMPL;
    }
    if (!out)
        return S_FALSE;
    auto* q = new Query12;
    q->desc = *desc;
    if (OcclusionQuery(q) || desc->Query == D3D10_QUERY_TIMESTAMP)
    {
        std::lock_guard<std::mutex> guard(m_queryLock);
        if (!m_queryReadback)
        {
            D3D12_QUERY_HEAP_DESC hd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQuerySlots, 0};
            g.dev->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_occlusionHeap));
            hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            g.dev->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&m_timestampHeap));
            // Final occlusion counts, timestamps, partial occlusion counts.
            m_queryReadback = CreateBufferResource(D3D12_HEAP_TYPE_READBACK, 3 * kQuerySlots * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST);
            void* p = nullptr;
            m_queryReadback->Map(0, nullptr, &p);
            m_queryCpu = static_cast<const uint64_t*>(p);
            for (int i = kQuerySlots - 1; i >= 0; i--)
            {
                m_freeOcclusion.push_back(i);
                m_freeTimestamp.push_back(i);
            }
        }
        std::vector<int>& free = OcclusionQuery(q) ? m_freeOcclusion : m_freeTimestamp;
        if (free.empty())
        {
            q->Release();
            LogLimited("queryfull", "out of query slots");
            return E_OUTOFMEMORY;
        }
        q->heapIndex = free.back();
        free.pop_back();
    }
    *out = q;
    return S_OK;
}

void Device::QueryBegin(Query12* q)
{
    if (!OcclusionQuery(q))
        return;
    if (std::find(g_activeQueries.begin(), g_activeQueries.end(), q) != g_activeQueries.end())
        return;
    m_list->BeginQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex);
    q->split = false;
    g_activeQueries.push_back(q);
    m_activeQueries = g_activeQueries.size();
}

// Around a submission: active occlusion queries end in the old list (their count so far resolved to the partial slot)
// and begin again in the new one.
void Device::SplitQueries(bool end)
{
    for (Query12* q : g_activeQueries)
    {
        const D3D12_QUERY_TYPE type = OcclusionType(q);
        if (!end)
        {
            m_list->BeginQuery(m_occlusionHeap, type, q->heapIndex);
            continue;
        }
        if (q->split)
            LogLimited("split2", "an occlusion query spans more than two submissions; its first part is lost");
        m_list->EndQuery(m_occlusionHeap, type, q->heapIndex);
        m_list->ResolveQueryData(m_occlusionHeap, type, q->heapIndex, 1, m_queryReadback, (2 * kQuerySlots + q->heapIndex) * sizeof(uint64_t));
        q->split = true;
    }
}

void Device::QueryEnd(Query12* q)
{
    if (OcclusionQuery(q))
    {
        const auto it = std::find(g_activeQueries.begin(), g_activeQueries.end(), q);
        if (it == g_activeQueries.end())
        {
            m_list->BeginQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex); // ended without a begin: an empty count
            q->split = false;
        }
        else
            g_activeQueries.erase(it);
        m_activeQueries = g_activeQueries.size();
        m_list->EndQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex);
        m_list->ResolveQueryData(m_occlusionHeap, OcclusionType(q), q->heapIndex, 1, m_queryReadback, q->heapIndex * sizeof(uint64_t));
    }
    else if (q->desc.Query == D3D10_QUERY_TIMESTAMP)
    {
        m_list->EndQuery(m_timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q->heapIndex);
        m_list->ResolveQueryData(m_timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q->heapIndex, 1, m_queryReadback,
                                 (kQuerySlots + q->heapIndex) * sizeof(uint64_t));
    }
    q->serial = g_mainSerial;
    q->ended = true;
}

HRESULT Device::QueryData(Query12* q, void* data, UINT size, UINT flags)
{
    if (!q->ended)
        return S_FALSE;
    if (q->serial >= g_mainSerial)
    {
        if (!(flags & D3D10_ASYNC_GETDATA_DONOTFLUSH))
            Submit();
        return S_FALSE;
    }
    PollDone();
    if (!SerialDone(q->serial))
        return S_FALSE;
    return QueryRead(q, data, size);
}

// The producer's GetData (D3DLayer) after the stream thread executed the query's End: no submit (the stream thread does
// that, Dx12QueryFlush) and the completed serial refreshed only when the lock is free.
HRESULT Device::QueryPeek(Query12* q, void* data, UINT size, UINT flags, uint64_t* flushSerial)
{
    *flushSerial = 0;
    if (!q->ended)
        return S_FALSE;
    if (q->serial >= __atomic_load_n(&g_mainSerial, __ATOMIC_ACQUIRE))
    {
        if (!(flags & D3D10_ASYNC_GETDATA_DONOTFLUSH))
            *flushSerial = q->serial;
        return S_FALSE;
    }
    if (!SerialDone(q->serial) && g.lock.try_lock())
    {
        Retire();
        g.lock.unlock();
    }
    if (!SerialDone(q->serial))
        return S_FALSE;
    return QueryRead(q, data, size);
}

HRESULT Device::QueryRead(Query12* q, void* data, UINT size)
{
    if (!data)
        return S_OK;
    if (size != q->GetDataSize())
        return E_INVALIDARG;
    switch (q->desc.Query)
    {
    case D3D10_QUERY_EVENT:
        *static_cast<BOOL*>(data) = TRUE;
        break;
    case D3D10_QUERY_OCCLUSION:
        *static_cast<UINT64*>(data) = m_queryCpu[q->heapIndex] + (q->split ? m_queryCpu[2 * kQuerySlots + q->heapIndex] : 0);
        break;
    case D3D10_QUERY_OCCLUSION_PREDICATE:
        *static_cast<BOOL*>(data) = m_queryCpu[q->heapIndex] != 0 || (q->split && m_queryCpu[2 * kQuerySlots + q->heapIndex] != 0);
        break;
    case D3D10_QUERY_TIMESTAMP:
        *static_cast<UINT64*>(data) = m_queryCpu[kQuerySlots + q->heapIndex];
        break;
    case D3D10_QUERY_TIMESTAMP_DISJOINT:
    {
        UINT64 frequency = 0;
        g.queue->GetTimestampFrequency(&frequency);
        *static_cast<D3D10_QUERY_DATA_TIMESTAMP_DISJOINT*>(data) = {frequency, FALSE};
        break;
    }
    default:
        return E_FAIL;
    }
    return S_OK;
}

void Device::QueryFree(Query12* q)
{
    const auto it = std::find(g_activeQueries.begin(), g_activeQueries.end(), q);
    if (it != g_activeQueries.end())
    {
        m_list->EndQuery(m_occlusionHeap, OcclusionType(q), q->heapIndex); // released while active
        g_activeQueries.erase(it);
        m_activeQueries = g_activeQueries.size();
    }
    if (q->heapIndex < 0)
        return;
    const int index = q->heapIndex;
    const bool occlusion = OcclusionQuery(q);
    DeferCall([this, index, occlusion] {
        std::lock_guard<std::mutex> guard(m_queryLock);
        (occlusion ? m_freeOcclusion : m_freeTimestamp).push_back(index);
    });
}

void STDMETHODCALLTYPE Query12::Begin()
{
    g_device->QueryBegin(this);
}

void STDMETHODCALLTYPE Query12::End()
{
    g_device->QueryEnd(this);
}

HRESULT STDMETHODCALLTYPE Query12::GetData(void* data, UINT size, UINT flags)
{
    return g_device->QueryData(this, data, size, flags);
}

void Query12::Destroy()
{
    g_device->QueryFree(this);
    Child::Destroy();
}

