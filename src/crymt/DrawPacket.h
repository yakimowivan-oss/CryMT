// Draw packets: a draw with its own constant buffer contents and, optionally, its own vertex and index buffers,
// recorded by the renderer replacement (ZPass.cpp) on the render thread and executed by the D3D12 back end in stream
// order, with the rest of the state the renderer has bound at that point. Buffers given by a packet are bound for
// its draw only; the bindings before it are restored afterwards, so the renderer's own binding caches stay right.
#pragma once
#include <cstdint>

struct DrawPacketHeader
{
    uint32_t count, start; // DrawIndexed(count, start, base)
    int32_t base;
    uint8_t constants;   // DrawPacketConstants records that follow first
    uint8_t streams;     // then DrawPacketStream records
    uint8_t indexBuffer; // then one DrawPacketIndex record when set
    uint8_t pad;
};

struct DrawPacketConstants
{
    uint8_t stage;    // the renderer's numbering: 0 VS, 1 PS, 2 GS
    uint8_t slot;     // constant buffer slot
    uint16_t vectors; // followed by vectors * 16 bytes: the buffer's contents for this draw
};

struct DrawPacketStream
{
    uint64_t buffer; // ID3D10Buffer* (null: unbound)
    uint32_t offset, stride, slot, pad;
};

struct DrawPacketIndex
{
    uint64_t buffer; // ID3D10Buffer*
    uint32_t format, offset;
};

constexpr uint32_t kDrawPacketMaxBytes = 4096;
constexpr uint32_t kDrawPacketMaxConstants = 4;
constexpr uint32_t kDrawPacketMaxStreams = 8;

// A draw group: consecutive objects drawing one mesh range after the engine drew the first of them, so shaders, states,
// streams and per-batch constants are bound. One command carries the per-object inputs; the stream thread computes each
// object's per-instance constants (ZPass.cpp) and executes one draw packet per object.
void ZPassExpandGroup(const uint8_t* payload);
void D3DLayerDrawGroup(const uint8_t* payload, uint32_t bytes);
