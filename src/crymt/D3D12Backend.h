// D3D12 back end for the renderer's D3D10 device (D3D12Backend.cpp), used by D3DLayer.cpp when CRYMT_D3D12=1.
#pragma once
#include <windows.h>
#include <d3d10.h>
#include <dxgi.h>

#include <cstdint>
#include <cstdio>

// The device the engine holds (D3DLayer.cpp: the wrapper, with the command stream recording its calls), or null: what a
// resource's GetDevice gives, so code that takes the device from a texture (D3DX10) goes the engine's way.
ID3D10Device* D3DLayerPublicDevice();
// The renderer's final device, implemented on D3D12; null if D3D12 cannot start (the caller then creates the real one).
ID3D10Device* Dx12CreateDevice(IDXGIAdapter* adapter, UINT flags);
// Whether a device given to the DXGI factory is ours.
bool Dx12OwnsDevice(IUnknown* device);
// The swap chain for our device: a flip-model D3D12 swap chain behind an IDXGISwapChain the renderer uses as before.
HRESULT Dx12CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
void Dx12WriteStats(FILE* f, double frames);
void Dx12ResetStats(); // test command crymt_statsreset: the census counts from the next Present on (one place, not the session)
struct DlssSceneInfo;
// Context thread: the scene is complete before HDR post-processing (DLSS runs here).
void Dx12SceneDone(const DlssSceneInfo* info);
// Context thread: the main camera of the frame being drawn (view and projection for GI).
void Dx12Camera(const DlssSceneInfo* info);
// DLSS is running (CRYMT_DLSS and Streamline started); the back buffer size, for the camera jitter.
bool Dx12DlssActive();
bool Dx12ScreenSize(uint32_t* width, uint32_t* height);
// Any thread: write that many frames from the next present to logs\play\key_<time>_w1_c<n>.ppm (the capture key).
void Dx12CaptureNow(int frames, int flags); // flags for the name: 1 GI, 2 DLAA jitter, 4 object velocities
void Dx12SetObjectMotion(bool on);           // the engine's object velocities in DLSS's motion vectors
// Context thread: a draw packet (DrawPacket.h) with the bound state.
void Dx12DrawPacket(const uint8_t* packet);
// Context thread: the renderer's modelview and projection matrices (16 floats each) changed before the next draw.
void Dx12PassMatrices(const float* matrices);
// GI wants them (it voxelizes the shadow map draws); read on the render thread.
bool Dx12WantsPassMatrices();
// Direct dynamic-buffer maps (D3D11-driver style), for the stream's producer (the recording thread):
// Map(WRITE_DISCARD) of one of our dynamic buffers takes fresh upload memory at once and returns it in `cpu` (null: out
// of memory); the buffer switches to it when the stream thread reaches Dx12ApplyDiscard with the same words. False for
// buffers that are not ours or not dynamic.
struct Dx12Discard
{
    uint64_t words[5];
    void* cpu;
};
bool Dx12DiscardNow(ID3D10Buffer* buffer, Dx12Discard* out);
// Producer side: the memory a Map(NO_OVERWRITE) of one of our dynamic buffers writes (its last DISCARD's); null for others.
void* Dx12ProducerMemory(ID3D10Buffer* buffer);
// Context thread: the switch recorded with Dx12DiscardNow's words.
void Dx12ApplyDiscard(ID3D10Buffer* buffer, const uint64_t* words);
void Dx12DirectMapStats(uint64_t* discards, uint64_t* bytes);
// Asynchronous queries (the stream's producer): once the stream thread has executed the query's End, its data if its
// list has completed (S_OK), else S_FALSE; *flushSerial > 0 when that list is still being recorded and the caller asked
// for a flush (the stream thread then submits it: Dx12QueryFlush). Thread-safe against the stream thread.
HRESULT Dx12QueryPoll(ID3D10Asynchronous* query, void* data, UINT size, UINT flags, uint64_t* flushSerial);
// Context thread: submits the list being recorded when it is the given one (a GetData asked for it).
void Dx12QueryFlush(uint64_t serial);
// Context thread: a pass name for frame debuggers (RenderDoc, PIX) begins; null or "" ends the innermost one.
void Dx12PassName(const char* text);
