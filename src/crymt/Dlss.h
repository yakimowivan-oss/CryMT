// DLSS through NVIDIA Streamline (Dlss.cpp), used by the D3D12 back end when CRYMT_DLSS=1.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdio>

#include "DlssScene.h"

namespace dlss
{
// Loads Streamline and gives it the device; true when DLSS can run on the adapter.
bool Init(ID3D12Device* device, const LUID& adapter, void (*log)(const char*));
bool Active();
// The command queue and swap chain go through Streamline's proxies so it sees every Present (manual hooking).
HRESULT CreateQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, ID3D12CommandQueue** queue);
IDXGIFactory4* ProxyFactory(IDXGIFactory4* factory); // AddRef'd; the factory itself when Streamline is off

struct Images
{
    ID3D12Resource* color; // scene HDR color, input
    D3D12_RESOURCE_STATES colorState;
    ID3D12Resource* depth;
    D3D12_RESOURCE_STATES depthState;
    ID3D12Resource* mvec; // motion vectors (screen units); zero when cameraMotion is false: Streamline derives it
    D3D12_RESOURCE_STATES mvecState;
    ID3D12Resource* output; // same size and format as color, unordered access
    D3D12_RESOURCE_STATES outputState;
    uint32_t width, height;
    bool cameraMotion; // mvec holds the full motion (CryMT's pass), not only the objects
    ID3D12Resource* exposure; // 1x1 R32F exposure (CryMT's pass), else null: DLSS's auto exposure
    D3D12_RESOURCE_STATES exposureState;
};

// DLAA on the list; the caller restores its own list state afterwards (descriptor heaps, root signature, pipeline).
bool Evaluate(ID3D12GraphicsCommandList* list, const Images& images, const DlssSceneInfo& scene);
void WriteStats(FILE* f);
} // namespace dlss
