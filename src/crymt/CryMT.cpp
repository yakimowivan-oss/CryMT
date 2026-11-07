// CryMT: renderer call gate and render thread for Crysis 1.2.1 (build 6156, Bin64, DX10).
// Replaces the IRenderer vtable of CryRenderD3D10.dll in memory. Mode 0 records what the main
// thread calls on the renderer, mode 1 runs EF_EndEf3D on a worker while main waits, mode 2 runs
// renderer work on a render thread from a command queue and waits only when main needs renderer state.
#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Crysis6156.h"
#include "Dlss.h"
#include "DlssScene.h"
#include "GiControl.h"
#include "Paths.h"
#include "RenderDocCapture.h"
#include "RendererClasses6156.h"
#include "Window.h"
#include "ZPass.h"
using namespace ce2;

// D3DLayer.cpp: the D3D10 layer, the command stream and the D3D12 back end.
void D3DLayerOnDll(const wchar_t* name, size_t chars, uintptr_t base);
void D3DLayerFrame(void* device);
void D3DLayerWriteStats();
bool D3DLayerPacketsAvailable(); // the renderer's device is on the D3D12 back end
void D3DLayerSceneDone(const DlssSceneInfo* info);
void D3DLayerCamera(const DlssSceneInfo* info);
void D3DLayerPassName(const char* text); // a pass name for frame debuggers, in stream order (null ends it)
bool D3DLayerDlssScreen(uint32_t* width, uint32_t* height);
void Dx12CaptureNow(int frames, int flags); // D3D12Backend.cpp: the capture key
void Dx12SetObjectMotion(bool on);           // D3D12Backend.cpp: object velocities in DLSS's motion vectors
bool D3DLayerScreen(uint32_t* width, uint32_t* height);
extern uint64_t g_gcDeferred;
extern uint64_t g_releaseDeferred;
void RunDeferredFrees();
extern uint64_t g_freesDeferred;
extern uint64_t g_posePrepared;
extern uint64_t g_poseHits;
extern uint64_t g_poseMisses;
extern uint64_t g_poseMissesLate;
struct LateMiss
{
    uintptr_t vtable, ret, obj;
    int lod;
};
constexpr uint64_t kLateMissLog = 16;
extern LateMiss g_lateMiss[kLateMissLog];
namespace
{
void NoteSyncUnderLock(int reason);
void TrackEngineLocks();
bool ReleaseEngineLockForWait();
void RetakeEngineLock();
int g_mainEngineLocks;           // engine critical sections held by main (TrackEngineLocks)
uint64_t g_engineLockWaits;       // main or the render thread waited for the other on the engine lock
uint64_t g_syncUnderLock;         // main waits for the render thread while holding one
int g_syncUnderLockReason[8];
uint64_t g_syncUnderLockCount[8];
} // namespace
void WriteCensus(FILE* f);
void WriteFillSyncSites(FILE* f);
void ProbeCompare();
void ProbeFlushStart();
void WriteProbe(FILE* f);
void FlushLightsBegin();
void CarryBack(bool frustumsOnly);
bool g_frustumTest; // CRYMT_FRUSTUMTEST, see ProbeSnapshot
void (*g_translateProxies)(uint64_t* regs, int nregs, uint64_t* stack, int nstack); // set by InstallProxyHooks
uint64_t g_flushLightsCopied; // fill mode light copies (FlushLightsBegin)
uint64_t g_lightIdChecks;     // EF_ADDDlight ids emulated by main and checked on the render thread
uint64_t g_lightIdMismatch;
const char* RendererMethodName(int method);
void WaitIdlePublic(int reason);
uint64_t g_particleDeferred;      // particle emitter/container destructions queued (PatchParticleDelete)
uint32_t g_innerFlagValue[8];     // flags of RenderWorld's inner render passes (RenderInnerHook)
uint64_t g_innerFlagCount[8];
extern std::vector<uintptr_t> g_frameObjects;
extern std::vector<uintptr_t> g_rtFrameObjects;
void PrepareSkinning(uintptr_t renderer);
void RunPendingGc();
void RunPendingMaterialDeletes();
void SnapshotTimer();

namespace
{
constexpr int kMaxSlots = 512;
// Methods of the other renderer classes (logs\mt\census_vtables.txt), numbered in file order; their sync
// points are reported as reason kMethodReasonBase + method number.
constexpr int kRendererMethodMax = 8192;
constexpr int kMethodReasonBase = 1024;
constexpr int kMaxSyncReasons = kMethodReasonBase + kRendererMethodMax;
constexpr size_t kStubSize = 32;
// Stack arguments carried by queued and replayed calls (ReplayCall copies this many); slot 126, the render
// mesh constructor at 0x381042E0, takes 17 arguments.
constexpr int kStackArgs = 24;
// Paths of the fault and hang reports, made before anything can fault (no allocation in the handlers).
std::string g_faultPath, g_hangPath;


// Renderer fields read by the shadowed getters.

enum Policy : uint8_t
{
    kSync = 0, // main waits for the render thread, then calls the renderer itself
    kDirect,   // reads state that queued work does not change
    kDefer,    // void call with arguments passed by value, replayed on the render thread
    kSpecial,
};

struct alignas(16) CallFrame
{
    uint64_t rcx, rdx, r8, r9;
    uint8_t xmm[4][16];
    uint64_t rax;
};
static_assert(offsetof(CallFrame, xmm) == 0x20 && offsetof(CallFrame, rax) == 0x60, "layout used by asm");

enum class Kind : uint32_t
{
    Call,
    DrawDynVB,
    ProxyGet,  // payload ProxyObj*: create the real temp object
    ProxyCall, // payload ProxySnap*: write the image into the real object, pass it in the given register
};

struct Command
{
    Kind kind;
    uint32_t slot;
    uint32_t mxcsr;
    uint32_t count;
    void* target;
    const void* payload;
    CallFrame regs;
    uint64_t stack[kStackArgs];
};

struct SlotStats
{
    uint64_t calls;
    uint64_t framesSeen;
    uint64_t firstSum;
    uint64_t stamp;
};

struct SyncStats
{
    uint64_t waits;
    uint64_t waitTicks;
    uint64_t first;
    uint64_t firstTicks;
};

void* g_orig[kMaxSlots];
uint8_t g_policy[kMaxSlots];
SlotStats g_slots[kMaxSlots];
SyncStats g_sync[kMaxSyncReasons];
int g_slotCount;
DWORD g_mainThread;
int g_depth;
int g_mode;
bool g_inWindow;
uint64_t g_frame;
int64_t g_windowStart;
uint64_t g_renderSum;
uint64_t g_windowSum;
uint64_t g_otherEndEf;
double g_qpcToMs;
char g_setupLog[2048];

struct TraceEntry
{
    uint32_t slot;
    uintptr_t ret;
    uint64_t arg;
    int64_t time;
};
constexpr int kMaxTrace = 20000;
TraceEntry g_trace[kMaxTrace];
int g_traceCount;
uint64_t g_traceFrame = 3000;
uint64_t g_queryHist[256];

// Render thread state (mode 2). The ring is written by main and read by the render thread.
constexpr uint64_t kRingSize = 8192;
constexpr size_t kArenaSize = 32 * 1024 * 1024;
Command g_ring[kRingSize];
std::atomic<uint64_t> g_read{0};
std::atomic<uint64_t> g_write{0};
std::atomic<int> g_rtSleeping{0};
HANDLE g_rtWake;
HANDLE g_rtThread;
bool g_async;
uint8_t* g_arena;
size_t g_arenaUsed;
const void* g_dynPtr;
uint32_t g_dynVerts;
uintptr_t g_renderer;
uint8_t g_shadowFog;
int g_shadowFrameId;
int g_shadowFrameIdAll;
int g_shadowLevel;
bool g_firstSyncSeen;
int64_t g_dispatchTime;
uint64_t g_mainWaitTicks;
std::atomic<uint64_t> g_rtBusyTicks{0};
std::atomic<uint64_t> g_rtEndEfTicks{0};
uint64_t g_commands;
uint64_t g_ringFullWaits;
uint64_t g_arenaFullWaits;
using CreateDeviceFn = HRESULT(WINAPI*)(void*, int, HMODULE, UINT, UINT, void**);
CreateDeviceFn g_realCreateDevice;
using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcFn g_realGetProcAddress;
int g_level = 2;

// ID3D10Device calls made by the main thread while the render thread may be running.
constexpr int kMaxDevSlots = 128;
constexpr int kMaxDevSites = 256;
void* g_devOrig[kMaxDevSlots];
int g_devSlotCount;
uint64_t g_devCallsOther; // device calls from threads other than main and the render thread (CRYMT_DEVGATE)
struct DevSite
{
    uint32_t method;
    uintptr_t ret;
    uint64_t count;
};
DevSite g_devSites[kMaxDevSites];
int g_devSiteCount;
uint64_t g_devCallsAsync;
DWORD g_rtTid;

// The flush calls back into Cry3DEngine (water reflections, FUN_300eefe0) only from the preprocess
// stage, FUN_380a6d20 called at 0x380AA61F. Main may run ahead once that call has returned.
int g_rtDepth;
uint64_t g_rtFrameSeq;
std::atomic<uint64_t> g_preDoneSeq{0};
int64_t g_preDoneTime;
uint64_t g_frameSeq;
uint64_t g_flushCmdIndex;
uint64_t g_preWaitTicks;
// Top-level flush phases on the render thread: start to the preprocess call, preprocess, shadow prep.
int64_t g_flushStartTick, g_phaseMark;
uint64_t g_phaseTicks[3], g_phaseFrames;
uint64_t g_shadowDrawTicks, g_shadowDraws; // nested flushes of PrepareDepthMap (the shadow map draws)
uint64_t g_preSignals;
uint64_t g_lateCallbacks;
uint64_t g_preFallbacks;
uint64_t g_warmupFrames = 60;
// Level change (C3DEngine UnloadLevel/LoadLevel .. PostLoadLevel + 30 frames): every frame waits for its flush.
bool g_levelChange;
uint64_t g_syncUntilFrame;

// Snapshot of the view state taken by the render thread when a flush starts. Getters that only read
// this state run the original renderer code against g_fake, a zeroed copy holding these fields.
constexpr size_t kFakeSize = 0x2E000;
// CD3D9Renderer is allocated with 0x2D8D4 bytes (0x380E7CB8) and placed 16-aligned at +0x14.
// CD3DRenderAuxGeom vtable (29 slots). Its methods are gated as indices kAuxBase + slot. Overloads
// appear in reverse declaration order (checked by decompiling every slot).
constexpr int kAuxBase = 300;

// Argument numbers count this as 0: 1..3 are rdx, r8, r9, 4.. are stack arguments.
struct ArgCopy
{
    int8_t arg;
    int8_t countArg; // -1: one element
    uint16_t elem;
};
struct AuxSpec
{
    bool copy;
    ArgCopy args[6];
};
constexpr uint16_t kVec3 = 12, kColor = 4, kIndex = 2;
const AuxSpec kAuxSpecs[kAuxSlots] = {
    {true, {{1, -1, 4}}},                                                     // 0 SetRenderFlags
    {false, {}},                                                              // 1 GetRenderFlags (sret)
    {true, {{1, -1, kVec3}, {2, -1, kColor}}},                                // 2 DrawPoint
    {true, {{1, 2, kVec3}, {3, 2, kColor}}},                                  // 3 DrawPoints, color per point
    {true, {{1, 2, kVec3}, {3, -1, kColor}}},                                 // 4 DrawPoints
    {true, {{1, -1, kVec3}, {2, -1, kColor}, {3, -1, kVec3}, {4, -1, kColor}}}, // 5 DrawLine
    {true, {{1, 2, kVec3}, {3, 4, kIndex}, {5, 2, kColor}}},                  // 6 DrawLines indexed, colors
    {true, {{1, 2, kVec3}, {3, 4, kIndex}, {5, -1, kColor}}},                 // 7 DrawLines indexed
    {true, {{1, 2, kVec3}, {3, 2, kColor}}},                                  // 8 DrawLines, colors
    {true, {{1, 2, kVec3}, {3, -1, kColor}}},                                 // 9 DrawLines
    {true, {{1, 2, kVec3}, {4, 2, kColor}}},                                  // 10 DrawPolyline, colors
    {true, {{1, 2, kVec3}, {4, -1, kColor}}},                                 // 11 DrawPolyline
    {true, {{1, -1, kVec3}, {2, -1, kColor}, {3, -1, kVec3}, {4, -1, kColor}, {5, -1, kVec3}, {6, -1, kColor}}}, // 12 DrawTriangle
    {true, {{1, 2, kVec3}, {3, 4, kIndex}, {5, 2, kColor}}},                  // 13 DrawTriangles indexed, colors
    {true, {{1, 2, kVec3}, {3, 4, kIndex}, {5, -1, kColor}}},                 // 14 DrawTriangles indexed
    {true, {{1, 2, kVec3}, {3, 2, kColor}}},                                  // 15 DrawTriangles, colors
    {true, {{1, 2, kVec3}, {3, -1, kColor}}},                                 // 16 DrawTriangles
    {true, {{1, -1, 24}, {2, -1, 48}, {4, -1, kColor}, {5, -1, 4}}},          // 17 DrawAABB with Matrix34
    {true, {{1, -1, 24}, {3, -1, kColor}, {4, -1, 4}}},                       // 18 DrawAABB
    {true, {{1, -1, 60}, {2, -1, 48}, {4, -1, kColor}, {5, -1, 4}}},          // 19 DrawOBB with Matrix34
    {true, {{1, -1, 60}, {2, -1, kVec3}, {4, -1, kColor}, {5, -1, 4}}},       // 20 DrawOBB with position
    {true, {{1, -1, kVec3}, {3, -1, kColor}}},                                // 21 DrawSphere
    {true, {{1, -1, kVec3}, {2, -1, kVec3}, {5, -1, kColor}}},                // 22 DrawCone
    {true, {{1, -1, kVec3}, {2, -1, kVec3}, {5, -1, kColor}}},                // 23 DrawCylinder
    {false, {}},                                                              // 24 DrawBone(Matrix34), empty
    {true, {{1, -1, 28}, {2, -1, 28}}},                                       // 25 DrawBone(QuatT)
    {false, {}},                                                              // 26 Flush
    {false, {}},                                                              // 27 GetMemoryUsage
    {false, {}},                                                              // 28 flush counter getter
};
// EF_Query ids that only read flags and settings; answered from values computed when the flush starts.
constexpr uint32_t kSnapQueries[] = {0x20, 0x23, 0x25, 0x26};
bool IsSnapQuery(uint32_t id)
{
    for (uint32_t q : kSnapQueries)
        if ((id & 0xFF) == q)
            return true;
    return false;
}
uint64_t g_querySnap[4];
uint64_t g_queryMismatch;
uint64_t g_queryHits;
uintptr_t g_auxObject;
uint32_t g_shadowAuxFlags;
uint64_t g_auxDeferred;

uint8_t* g_fake;
uint8_t* g_fakeMvStack;
uint8_t* g_fakeProjStack;
alignas(16) float g_snapMv[16];
alignas(16) float g_snapProj[16];
std::atomic<uint64_t> g_snapSeq{0};
bool g_snapInvalid;
bool g_snapMatrixInvalid;
int g_matrixDepth;
uint64_t g_snapHits;
uint64_t g_snapMismatch;
uint64_t g_snapChecks;
int g_invalidReason;
uint64_t g_bfChanged[4];

// Fill window view state: at dispatch, with the render thread idle, the whole renderer object is copied into
// the fake together with its two matrix stacks (header: top, base, depth, capacity, flags; the base array is
// allocated once by FUN_380F0840 and never grown). In the window, view setters run on the fake for main and are
// queued for the render thread ("mirrored"), so main's getters see the state it set.
struct MatrixStack
{
    float* top;
    float* base;
    int depth;
    int capacity;
    int flags;
    int pad;
};
MatrixStack g_fakeStacks[2];
std::vector<float> g_fakeStackData[2];
bool g_mirroring;
bool g_fakeOwned; // fill mode: the fake holds main's view of the renderer from one dispatch to the next
uint64_t g_mirrorCalls;
uint64_t g_mirrorChecks;
uint64_t g_mirrorMismatch;
uint64_t g_mirrorMethodCalls; // a mirrored setter reached another renderer object (not expected)

void CopyRendererToFake(const uint8_t* renderer)
{
    memcpy(g_fake, renderer, kRendererObjectSize);
    const uintptr_t offsets[2] = {kModelViewStackOffset, kProjStackOffset};
    for (int k = 0; k < 2; k++)
    {
        const auto* real = *reinterpret_cast<const MatrixStack* const*>(renderer + offsets[k]);
        MatrixStack& fake = g_fakeStacks[k];
        fake = *real;
        g_fakeStackData[k].resize(static_cast<size_t>(real->capacity) * 16);
        memcpy(g_fakeStackData[k].data(), real->base, static_cast<size_t>(real->capacity) * 64);
        fake.base = g_fakeStackData[k].data();
        fake.top = fake.base + static_cast<size_t>(real->depth) * 16;
        *reinterpret_cast<MatrixStack**>(g_fake + offsets[k]) = &fake;
    }
    g_snapInvalid = false;
    g_fakeOwned = true;
}

// What a mirrored setter left in the fake, checked by the render thread after the real call.
struct MirrorExpect
{
    uint8_t camera[0x80];
    float mv[16];
    float proj[16];
    int viewport[4];
};

void MirrorCapture(const uint8_t* renderer, MirrorExpect& e)
{
    const auto top = [&](uintptr_t off) { return (*reinterpret_cast<const MatrixStack* const*>(renderer + off))->top; };
    memcpy(e.camera, renderer + kCameraOffset, sizeof(e.camera));
    memcpy(e.mv, top(kModelViewStackOffset), 64);
    memcpy(e.proj, top(kProjStackOffset), 64);
    memcpy(e.viewport, renderer + kViewportOffset, sizeof(e.viewport));
}

// Render object proxies (fill window). EF_GetObject(temp) hands main a proxy from its own pool, initialized like
// a temp object; the render thread creates the real object when the command reaches it. CRenderObject slot 0
// (two 0x14-byte shader parameter entries in the frame array 0x382E9B88, index at +0xD2) and slot 1 (GetObjData,
// 0x20 bytes, TArray at +0x18) give a proxy its own copies. CRenderMesh::Render and EF_AddEf on a proxy are queued
// with an image of the proxy taken at the call; the render thread writes it into the real object (ids at
// +0x8..+0xF and the entry index +0xD2 stay the renderer's) before the original call.
struct alignas(16) ProxyObj
{
    uint8_t obj[kRenderObjectBytes];
    uint8_t entries[kObjEntryBytes];
    uint8_t objData[kObjDataBytes];
    uint8_t* real;
    bool hasEntries;
    bool hasObjData;
};
struct ProxySnap
{
    ProxyObj* proxy;
    int objReg; // argument holding the object: 2 = r8 (CRenderMesh::Render), 3 = r9 (EF_AddEf), 4+ = stack
    bool hasEntries;
    bool hasObjData;
    uint8_t obj[kRenderObjectBytes];
    uint8_t entries[kObjEntryBytes];
    uint8_t objData[kObjDataBytes];
};
constexpr size_t kProxyMax = 8192;
ProxyObj* g_proxies; // two halves, alternating per frame
int g_proxyHalf;
size_t g_proxyUsed;
uint64_t g_proxyCount;
// Stack arguments of a renderer call that can hold a render object. Past a call's real arguments the stack slots
// are the caller's locals (often proxies), which must neither trigger a sync nor be rewritten. Of the IRenderer
// methods only EF_AddPolygonToScene (92: object is argument 5) takes one on the stack; aux geometry takes none.
inline int ProxyStackArgs(uint32_t idx)
{
    return idx == kAddPolygon ? 6 : 0;
}
uint64_t g_polysQueued;
uint64_t g_meshIndicesQueued; // CRenderMesh::SetIndices calls queued in the fill window
uint64_t g_meshChunksQueued;  // CRenderMesh::SetChunk calls
uint64_t g_resourceAddRefsQueued; // CShader/CTexture AddRef and AddDynamicLightSource's releases from the fill window
uint64_t g_resourceReleasesQueued;
std::atomic<HANDLE> g_saveWriter; // save file being written (InstallAsyncSave)
std::atomic<uint64_t> g_saveWrites, g_saveWriteFailures, g_saveWriteTicks, g_saveWaits;
bool g_saveWriterOn;
uint64_t g_texUpdatesQueued, g_texUpdateChecks, g_texUpdateMismatch; // DownLoadToVideoMemory with an existing id
uint64_t g_occlusionReadsQueued; // CREOcclusionQuery::mfReadResult_Try
// Shadow caster recording (CRYMT_SHADOWREC, fill mode). Shadow prep (PrepareDepthMap, vf207) calls every caster's
// Render on the render thread, which runs 3D engine and game code there while main is held. Instead main renders
// each caster of each shadow frustum at dispatch, while the render thread is idle, with the SRendParams vf207 builds;
// the renderer calls this makes are recorded rather than queued, and vf207's caster call (CasterRenderHook) replays
// the caster's recording in the shadow pass it set up.
struct CasterRecording
{
    uintptr_t caster, frustum; // frustum: main's original (the flush draws copies, see FlushLightsBegin)
    uint32_t first, count;     // commands in g_recCommands
    float distance;
};
bool g_shadowRec;
bool g_recordingNow; // BeginCommand appends to g_recCommands, PublishCommand does nothing
std::vector<CasterRecording> g_recordings;
std::vector<Command> g_recCommands;
std::unordered_map<uint64_t, uint32_t> g_recIndex; // (caster, frustum) -> g_recordings index
uint64_t g_recCasters, g_recReplays, g_recFallbacks, g_recParamMismatch;
// Terrain mesh ownership (CRYMT_TERRAINOWN, fill mode). A terrain sector update that would edit the mesh the render
// thread may still draw recreates it instead (TerrainReuseHook). Main creates meshes without a sync (the global mesh
// list is guarded by g_meshListCs in the constructor and destructor hooks) and edits the meshes it created in this
// fill window directly, until a queued command uses them (HandOffMesh) or the frame is dispatched. Their chunks'
// render elements come from a pool made at dispatch, when the render thread is idle (CRendElement constructors link
// every element into a global list).
bool g_terrainOwn;
bool g_meshListHooked;
bool g_inMeshCreate;
std::vector<uintptr_t> g_mainMeshes;
std::vector<void*> g_rePool;
constexpr size_t kRePoolSize = 256; // island benchmark: about 13 a frame, peaks above 96
uint64_t g_mainMeshesCreated, g_mainMeshCalls, g_mainMeshHandoffs, g_terrainRebuilds, g_rePoolUsed, g_rePoolEmpty;
bool g_terrainTexQueue;     // TerrainTexturesHook
uint64_t g_terrainTexQueued;
uint64_t g_surfacesRecreated;

bool IsMainMesh(uintptr_t mesh)
{
    for (uintptr_t m : g_mainMeshes)
        if (m == mesh)
            return true;
    return false;
}

void HandOffMesh(uintptr_t mesh)
{
    for (size_t i = 0; i < g_mainMeshes.size(); i++)
        if (g_mainMeshes[i] == mesh)
        {
            g_mainMeshes[i] = g_mainMeshes.back();
            g_mainMeshes.pop_back();
            g_mainMeshHandoffs++;
            return;
        }
}
// Permanent render objects by id (EF_GetObject(false, -1) results, id at +8). An id keeps its object: freed ones
// (flag 0x2000) are reused in place. Main answers EF_GetObject(false, id) in the fill window from here.
std::atomic<void*> g_permObjects[kPermObjectMax]; // also written by the render thread (EF_ADDDlight light objects)
uint64_t g_permQueued;
uint64_t g_levelLightQueries;
uint32_t g_fogContribs; // main's count of the renderer's fog volume contributions this frame
uint64_t g_fogQueued, g_fogChecks, g_fogMismatch;
struct ParticleGs // EF_AddParticlesToScene's canUseGS in the fill window
{
    uint8_t value;  // what the render thread passes and gets back
    uint8_t expect; // main's answer
};
uint64_t g_particlesQueued, g_particleGsChecks, g_particleGsMismatch;
uint64_t g_proxyApplied;
uint64_t g_proxyTranslated;
uint64_t g_proxyDataArrays; // proxies whose objdata TArray main filled (not carried)
using ObjSlot0Fn = void(__fastcall*)(void*, uint8_t**);
using ObjSlot1Fn = uint8_t*(__fastcall*)(void*, char);
using GetObjectRealFn = uint8_t*(__fastcall*)(void*, uint64_t, uint64_t, uint64_t);
uintptr_t g_objSlot0;
uintptr_t g_objSlot1;
uintptr_t g_realGetObject; // EF_GetObject itself (set by InstallGate)
uint64_t g_proxyQueryHist[256]; // EF_Query ids called with a proxy argument

// Pointers into the proxy pool: the object itself, or its entries / object data handed out by slots 0 and 1.
bool InProxyPool(uint64_t p)
{
    return g_proxies && p >= reinterpret_cast<uint64_t>(g_proxies) && p < reinterpret_cast<uint64_t>(g_proxies + 2 * kProxyMax);
}

bool IsProxy(uint64_t p)
{
    return InProxyPool(p) && (p - reinterpret_cast<uint64_t>(g_proxies)) % sizeof(ProxyObj) == 0;
}

ProxyObj* ProxyContaining(uint64_t p)
{
    return g_proxies + (p - reinterpret_cast<uint64_t>(g_proxies)) / sizeof(ProxyObj);
}

// The real object's two shader parameter entries. Slot 0 allocates them when +0xD2 is 0; its reuse branch
// (+0xD2 set) reads the index from an uninitialized stack slot ([rsp+0x30] at 0x38100892), so it is called
// once and the entries are found from the index (array 0x382E9B88, entries idx and idx + 1).
uint8_t* RealEntries(uint8_t* real)
{
    if (*reinterpret_cast<int16_t*>(real + 0xD2) == 0)
    {
        uint8_t* out[2] = {};
        reinterpret_cast<ObjSlot0Fn>(g_objSlot0)(real, out);
    }
    const int idx = *reinterpret_cast<int16_t*>(real + 0xD2);
    auto* base = *reinterpret_cast<uint8_t**>(kObjEntryArray);
    return idx > 0 && base ? base + static_cast<size_t>(idx) * 0x14 : nullptr;
}

// The real temp object behind a proxy, created at its first use by whoever runs the renderer at that moment (the
// render thread, or main after a sync point).
uint8_t* RealOf(ProxyObj* p)
{
    if (!p->real && g_realGetObject)
        p->real = reinterpret_cast<GetObjectRealFn>(g_realGetObject)(reinterpret_cast<void*>(g_renderer), 1, 0xFFFFFFFFu, 0);
    return p->real;
}

// Render thread (or main with the render thread idle): write a proxy image into the real object.
void ApplyProxyImage(ProxyObj* p, const uint8_t* obj, bool hasEntries, const uint8_t* entries, bool hasObjData, const uint8_t* objData)
{
    uint8_t* real = RealOf(p);
    if (!real)
        return;
    memcpy(real + 0x10, obj + 0x10, 0xD2 - 0x10);
    memcpy(real + 0xD4, obj + 0xD4, kRenderObjectBytes - 0xD4);
    if (hasEntries)
        if (uint8_t* e = RealEntries(real))
            memcpy(e, entries, kObjEntryBytes);
    if (hasObjData)
        if (uint8_t* od = reinterpret_cast<ObjSlot1Fn>(g_objSlot1)(real, 1))
            memcpy(od, objData, 0x18);
    g_proxyApplied++;
}

void ApplyProxyLive(ProxyObj* p)
{
    ApplyProxyImage(p, p->obj, p->hasEntries, p->entries, p->hasObjData, p->objData);
}

uint64_t g_proxyQueuedTranslated;

// Render thread, queued call: pointers into proxies become pointers into the real objects (created by the
// earlier ProxyGet command); the object contents arrive with the Render/AddEf images.
void TranslateQueued(uint64_t* regs, int nregs, uint64_t* stack, int nstack)
{
    for (int i = 0; i < nregs + nstack; i++)
    {
        uint64_t& v = i < nregs ? regs[i] : stack[i - nregs];
        if (!InProxyPool(v))
            continue;
        ProxyObj* p = ProxyContaining(v);
        const uint64_t off = v - reinterpret_cast<uint64_t>(p);
        if (!RealOf(p))
            continue;
        if (off < kRenderObjectBytes)
            v = reinterpret_cast<uint64_t>(p->real + off);
        else if (off < offsetof(ProxyObj, entries) + kObjEntryBytes)
        {
            uint8_t* e = RealEntries(p->real);
            if (!e)
                continue;
            v = reinterpret_cast<uint64_t>(e + (off - offsetof(ProxyObj, entries)));
        }
        else if (off < offsetof(ProxyObj, objData) + kObjDataBytes)
        {
            uint8_t* od = reinterpret_cast<ObjSlot1Fn>(g_objSlot1)(p->real, 1);
            if (!od)
                continue;
            v = reinterpret_cast<uint64_t>(od + (off - offsetof(ProxyObj, objData)));
        }
        else
            continue;
        g_proxyQueuedTranslated++;
    }
}

// SetViewport defines the viewport only; the matrices may differ until the next SetCamera (BeginFrame).
bool MirrorMatches(const uint8_t* renderer, const MirrorExpect& e, bool viewportOnly)
{
    MirrorExpect now;
    MirrorCapture(renderer, now);
    if (viewportOnly)
        return memcmp(now.viewport, e.viewport, sizeof(now.viewport)) == 0;
    return memcmp(&now, &e, sizeof(now)) == 0;
}
int64_t g_missReason[16][2];
uint64_t g_boundaryChecks;
uint64_t g_boundaryMismatch;
uint64_t g_endFrameMismatch;

// Nested EF_EndEf3D calls made from inside the top-level flush (mode 1 measurement).
constexpr int kNestedDepth = 14;
constexpr int kMaxNested = 64;
struct NestedSite
{
    void* frames[kNestedDepth];
    int depth;
    uint64_t count;
    uint64_t startTicks;
    uint64_t durTicks;
};
NestedSite g_nested[kMaxNested];
int g_nestedCount;
int64_t g_topStart;
uint64_t g_topTicks;
uint64_t g_topCount;
uint64_t g_lastNestedEndTicks;
int64_t g_frameLastNestedEnd;
uint64_t g_endEfCalls;
uint64_t g_endEfTop;
int g_lastSyncSlot = -1;
uintptr_t g_lastEndEfRet;

int64_t Now()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

void SetupLog(const char* text)
{
    strncat_s(g_setupLog, text, _TRUNCATE);
    strncat_s(g_setupLog, "\n", _TRUNCATE);
}

// The build CryMT is made for (Crysis 1.2.1 64-bit, 6156): the modules it patches, by PE time stamp, image size and
// load address. Any other build, the DX9 renderer or a Crysis.exe without c1-launcher v8's memory exports turns
// CryMT off before it patches anything.
struct KnownModule
{
    const wchar_t* name;
    DWORD stamp, size;
    uintptr_t base;
};
constexpr KnownModule kKnownModules[] = {
    {L"CrySystem.dll", 0x47D6E0A0, 0x2FC000, 0x36500000},
    {L"CryRenderD3D10.dll", 0x47D6DBAA, 0xA9E000, 0x38000000},
    {L"Cry3DEngine.dll", 0x47D6E566, 0x25C000, 0x30000000},
    {L"CryAnimation.dll", 0x47D6E4D7, 0x1BC000, 0x31500000},
};
bool g_off;
char g_offReason[160];

void TurnOff(const char* reason)
{
    if (g_off)
        return;
    g_off = true;
    strcpy_s(g_offReason, reason);
    SetupLog(reason);
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_off.txt").c_str(), "w") == 0 && f)
    {
        fprintf(f, "%s\n", reason);
        fclose(f);
    }
}

bool NameIs(const UNICODE_STRING* n, const wchar_t* name)
{
    const size_t len = wcslen(name);
    return n->Length == len * 2 && _wcsnicmp(n->Buffer, name, len) == 0;
}

// False (and CryMT off) when `n` is one of the patched modules but not the 6156 build at its usual address.
bool ModuleMatches(const UNICODE_STRING* n, uintptr_t base)
{
    for (const KnownModule& m : kKnownModules)
    {
        if (!NameIs(n, m.name))
            continue;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        if (base == m.base && nt->FileHeader.TimeDateStamp == m.stamp && nt->OptionalHeader.SizeOfImage == m.size)
            return true;
        char reason[160];
        sprintf_s(reason, "CryMT off: %ls is not the Crysis 1.2.1 64-bit build 6156 module", m.name);
        TurnOff(reason);
        return false;
    }
    return true;
}

// Replaces a string literal in a module with one of the same length, when it still holds the expected text.
void PatchText(uintptr_t at, const char* expect, const char* text)
{
    const size_t n = strlen(expect) + 1;
    if (strlen(text) + 1 != n || memcmp(reinterpret_cast<const void*>(at), expect, n) != 0)
        return;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(at), text, n);
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
}

constexpr wchar_t kVersion[] = L"v13";

// What CryMT runs, for the window title (" | CryMT v13: D3D12 + DLAA, render thread"), or why it is off.
std::wstring ModeText()
{
    wchar_t text[224];
    if (g_off)
        swprintf_s(text, L" | %hs", g_offReason);
    else if (g_mode != 2)
        return {};
    else
    {
        const bool d3d12 = D3DLayerPacketsAvailable();
        const wchar_t* aa = !d3d12 || !GetEnvironmentVariableW(L"CRYMT_DLSS", nullptr, 0) ? L""
                            : dlss::Active()                                       ? L" + DLAA"
                                                                                   : L" (DLAA unavailable)";
        swprintf_s(text, L" | CryMT %ls: %ls%ls, render thread", kVersion, d3d12 ? L"D3D12" : L"D3D10", aa);
    }
    return text;
}

BOOL CALLBACK FindGameWindow(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER))
        return TRUE;
    *reinterpret_cast<HWND*>(out) = w;
    return FALSE;
}

// Keeps the mode in the game window's title. The engine sets the title when it creates the window, so the suffix is
// put back whenever it is missing or out of date; InternalGetWindowText reads it without a message to main.
// The crash reporter (CryMTReport.exe next to CryMT.dll, report\Report.cpp) waits for the game outside the process and
// reports an abnormal exit.
void StartReporter()
{
    const std::wstring dir = CryMTDirW();
    const std::wstring exe = dir + L"CryMTReport.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH); // ...\Bin64\Crysis.exe
    std::wstring root(path, n);
    root.resize(root.find_last_of(L'\\'));
    root.resize(root.find_last_of(L'\\') + 1);
    // Directories end with a backslash, doubled before the closing quote so it does not escape it.
    std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" \"" + dir + L"\\\" \"" + root + L"\\\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, dir.c_str(), &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

DWORD WINAPI TitleMain(void*)
{
    StartReporter();
    for (int tick = 0;; tick++)
    {
        Sleep(250);
        BorderlessApply();
        if (tick % 4)
            continue;
        HWND w = nullptr;
        EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&w));
        const std::wstring suffix = ModeText();
        if (!w || suffix.empty())
            continue;
        wchar_t title[512] = {};
        InternalGetWindowText(w, title, 512);
        std::wstring t = title;
        const size_t at = t.find(L" | CryMT");
        if (at != std::wstring::npos)
        {
            if (t.compare(at, std::wstring::npos, suffix) == 0)
                continue;
            t.resize(at);
        }
        t += suffix;
        SendMessageTimeoutW(w, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(t.c_str()), SMTO_ABORTIFHUNG, 2000, nullptr);
    }
}

void DlssWriteStats(FILE* f);

void WriteStats()
{
    D3DLayerWriteStats();
    zpass::WriteStats();
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_stats.txt").c_str(), "w") != 0 || !f)
        return;
    const double frames = g_frame ? static_cast<double>(g_frame) : 1.0;
    fprintf(f, "%s", g_setupLog);
    DlssWriteStats(f);
    fprintf(f, "frames=%llu avgRenderMs=%.3f avgWindowMs=%.3f otherEndEfCalls=%llu slots=%d\n",
            static_cast<unsigned long long>(g_frame), g_renderSum * g_qpcToMs / frames,
            g_windowSum * g_qpcToMs / frames, static_cast<unsigned long long>(g_otherEndEf), g_slotCount);
    fprintf(f, "rtThread=%lu\n", g_rtTid);
    if (g_topCount)
    {
        fprintf(f, "worker flush avgMs=%.3f, last nested pass ends at avgMs=%.3f\n", g_topTicks * g_qpcToMs / g_topCount,
                g_lastNestedEndTicks * g_qpcToMs / g_topCount);
        for (int i = 0; i < g_nestedCount; i++)
        {
            const NestedSite& n = g_nested[i];
            fprintf(f, "nested x%.2f/frame startMs=%.3f durMs=%.3f stack:", static_cast<double>(n.count) / g_topCount,
                    n.startTicks * g_qpcToMs / n.count, n.durTicks * g_qpcToMs / n.count);
            for (int k = 0; k < n.depth; k++)
            {
                HMODULE mod = nullptr;
                char name[MAX_PATH] = "?";
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(n.frames[k]), &mod);
                if (mod)
                    GetModuleFileNameA(mod, name, sizeof(name));
                const char* base = strrchr(name, '\\');
                fprintf(f, " %s+0x%llx", base ? base + 1 : name,
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(n.frames[k]) - reinterpret_cast<uintptr_t>(mod)));
            }
            fprintf(f, "\n");
        }
    }
    fprintf(f, "endEfCalls=%llu top=%llu lastRet=%llx mainThread=%lu depth=%d async=%d read=%llu write=%llu lastSync=%d\n",
            static_cast<unsigned long long>(g_endEfCalls), static_cast<unsigned long long>(g_endEfTop),
            static_cast<unsigned long long>(g_lastEndEfRet), g_mainThread, g_depth, g_async ? 1 : 0,
            static_cast<unsigned long long>(g_read.load()), static_cast<unsigned long long>(g_write.load()), g_lastSyncSlot);
    if (g_mode == 2)
    {
        fprintf(f, "preprocessWaitMs/frame=%.3f preprocessSignals=%llu fallbacks=%llu lateEngineCallbacks=%llu gcDeferred=%llu\n",
                g_preWaitTicks * g_qpcToMs / frames, static_cast<unsigned long long>(g_preSignals),
                static_cast<unsigned long long>(g_preFallbacks), static_cast<unsigned long long>(g_lateCallbacks),
                static_cast<unsigned long long>(g_gcDeferred));
        if (g_phaseFrames)
            fprintf(f, "flush phases ms/frame: start to preprocess %.3f, preprocess %.3f, shadow prep %.3f (shadow map draws %.3f, %.2f per frame)\n",
                    g_phaseTicks[0] * g_qpcToMs / g_phaseFrames, g_phaseTicks[1] * g_qpcToMs / g_phaseFrames,
                    g_phaseTicks[2] * g_qpcToMs / g_phaseFrames, g_shadowDrawTicks * g_qpcToMs / g_phaseFrames,
                    static_cast<double>(g_shadowDraws) / g_phaseFrames);
        fprintf(f, "viewSnapshot hits=%llu checks=%llu mismatches=%llu auxDeferred=%llu\n", static_cast<unsigned long long>(g_snapHits),
                static_cast<unsigned long long>(g_snapChecks), static_cast<unsigned long long>(g_snapMismatch),
                static_cast<unsigned long long>(g_auxDeferred));
        fprintf(f, "fill mirror: view setters mirrored=%llu checked=%llu mismatches=%llu other renderer objects reached=%llu\n",
                static_cast<unsigned long long>(g_mirrorCalls), static_cast<unsigned long long>(g_mirrorChecks),
                static_cast<unsigned long long>(g_mirrorMismatch), static_cast<unsigned long long>(g_mirrorMethodCalls));
        fprintf(f, "fill lights: flushes with copies=%llu light ids checked=%llu mismatches=%llu\n",
                static_cast<unsigned long long>(g_flushLightsCopied), static_cast<unsigned long long>(g_lightIdChecks),
                static_cast<unsigned long long>(g_lightIdMismatch));
        fprintf(f, "fill proxies: created=%llu images applied=%llu translated at sync points=%llu objdata arrays not carried=%llu\n",
                static_cast<unsigned long long>(g_proxyCount), static_cast<unsigned long long>(g_proxyApplied),
                static_cast<unsigned long long>(g_proxyTranslated), static_cast<unsigned long long>(g_proxyDataArrays));
        fprintf(f, "fill polygons queued=%llu, permanent objects queued=%llu, level light queries answered=%llu\n",
                static_cast<unsigned long long>(g_polysQueued), static_cast<unsigned long long>(g_permQueued),
                static_cast<unsigned long long>(g_levelLightQueries));
        fprintf(f, "fill fog contributions queued=%llu checked=%llu mismatches=%llu\n", static_cast<unsigned long long>(g_fogQueued),
                static_cast<unsigned long long>(g_fogChecks), static_cast<unsigned long long>(g_fogMismatch));
        fprintf(f, "fill mesh SetIndices queued=%llu SetChunk queued=%llu, resource AddRef/Release queued=%llu (releases %llu)\n",
                static_cast<unsigned long long>(g_meshIndicesQueued), static_cast<unsigned long long>(g_meshChunksQueued),
                static_cast<unsigned long long>(g_resourceAddRefsQueued), static_cast<unsigned long long>(g_resourceReleasesQueued));
        if (g_saveWrites)
            fprintf(f, "async save: writes %llu (failed %llu), %.1f ms each on the worker, waited for %llu times\n",
                    static_cast<unsigned long long>(g_saveWrites.load()), static_cast<unsigned long long>(g_saveWriteFailures.load()),
                    g_saveWriteTicks.load() * g_qpcToMs / g_saveWrites.load(), static_cast<unsigned long long>(g_saveWaits.load()));
        fprintf(f, "texture updates queued=%llu checked=%llu mismatches=%llu, occlusion reads queued=%llu\n",
                static_cast<unsigned long long>(g_texUpdatesQueued), static_cast<unsigned long long>(g_texUpdateChecks),
                static_cast<unsigned long long>(g_texUpdateMismatch), static_cast<unsigned long long>(g_occlusionReadsQueued));
        if (g_shadowRec)
            fprintf(f, "shadow recording: casters recorded=%llu replays=%llu fallbacks=%llu parameter mismatches=%llu\n",
                    static_cast<unsigned long long>(g_recCasters), static_cast<unsigned long long>(g_recReplays),
                    static_cast<unsigned long long>(g_recFallbacks), static_cast<unsigned long long>(g_recParamMismatch));
        if (g_terrainOwn)
            fprintf(f, "terrain ownership: rebuilds=%llu surfaces=%llu meshes created on main=%llu direct calls=%llu handoffs=%llu pooled REs used=%llu pool empty=%llu\n",
                    static_cast<unsigned long long>(g_terrainRebuilds), static_cast<unsigned long long>(g_surfacesRecreated),
                    static_cast<unsigned long long>(g_mainMeshesCreated),
                    static_cast<unsigned long long>(g_mainMeshCalls), static_cast<unsigned long long>(g_mainMeshHandoffs),
                    static_cast<unsigned long long>(g_rePoolUsed), static_cast<unsigned long long>(g_rePoolEmpty));
        fprintf(f, "terrain texture ids queued=%llu\n", static_cast<unsigned long long>(g_terrainTexQueued));
        fprintf(f, "fill particles queued=%llu canUseGS checked=%llu mismatches=%llu\n", static_cast<unsigned long long>(g_particlesQueued),
                static_cast<unsigned long long>(g_particleGsChecks), static_cast<unsigned long long>(g_particleGsMismatch));
        fprintf(f, "EF_Query ids with a proxy argument:");
        for (int q = 0; q < 256; q++)
            if (g_proxyQueryHist[q])
                fprintf(f, " %x x%llu", q, static_cast<unsigned long long>(g_proxyQueryHist[q]));
        fprintf(f, "\n");
        fprintf(f, "engine lock waits=%llu, syncs while main holds an engine lock=%llu, reasons:",
                static_cast<unsigned long long>(g_engineLockWaits), static_cast<unsigned long long>(g_syncUnderLock));
        for (int i = 0; i < 8 && g_syncUnderLockCount[i]; i++)
            fprintf(f, " %d x%llu", g_syncUnderLockReason[i], static_cast<unsigned long long>(g_syncUnderLockCount[i]));
        fprintf(f, "\n");
        fprintf(f, "particle destructions deferred=%llu, inner render flags:", static_cast<unsigned long long>(g_particleDeferred));
        for (int i = 0; i < 8 && g_innerFlagCount[i]; i++)
            fprintf(f, " %x x%llu", g_innerFlagValue[i], static_cast<unsigned long long>(g_innerFlagCount[i]));
        fprintf(f, "\n");
        fprintf(f, "freesDeferred=%llu posesPrepared=%llu poseHits=%llu poseMisses=%llu (after main released %llu)\n",
                static_cast<unsigned long long>(g_freesDeferred), static_cast<unsigned long long>(g_posePrepared),
                static_cast<unsigned long long>(g_poseHits), static_cast<unsigned long long>(g_poseMisses),
                static_cast<unsigned long long>(g_poseMissesLate));
        for (uint64_t i = 0; i < std::min(g_poseMissesLate, kLateMissLog); i++)
            fprintf(f, "  late pose miss: skin vtable %llx caller %llx object %llx lod %d\n",
                    static_cast<unsigned long long>(g_lateMiss[i].vtable), static_cast<unsigned long long>(g_lateMiss[i].ret),
                    static_cast<unsigned long long>(g_lateMiss[i].obj), g_lateMiss[i].lod);
        fprintf(f, "after BeginFrame changed: viewport=%llu camera=%llu modelview=%llu projection=%llu\n",
                static_cast<unsigned long long>(g_bfChanged[0]), static_cast<unsigned long long>(g_bfChanged[1]),
                static_cast<unsigned long long>(g_bfChanged[2]), static_cast<unsigned long long>(g_bfChanged[3]));
        fprintf(f, "snapshot misses by reason (slot, 1000+sync slot, 2000+matrix depth):");
        for (int k = 0; k < 16 && g_missReason[k][1]; k++)
            fprintf(f, " %lld=%lld", static_cast<long long>(g_missReason[k][0]), static_cast<long long>(g_missReason[k][1]));
        fprintf(f, "\n");
        fprintf(f, "querySnapshot hits=%llu mismatches=%llu releasesDeferred=%llu frameBoundaryChecks=%llu mismatches=%llu (after EndFrame %llu)\n",
                static_cast<unsigned long long>(g_queryHits), static_cast<unsigned long long>(g_queryMismatch),
                static_cast<unsigned long long>(g_releaseDeferred), static_cast<unsigned long long>(g_boundaryChecks),
                static_cast<unsigned long long>(g_boundaryMismatch), static_cast<unsigned long long>(g_endFrameMismatch));
        fprintf(f, "mainWaitMs/frame=%.3f rtBusyMs/frame=%.3f rtEndEf3DMs/frame=%.3f commands/frame=%.1f ringFull=%llu arenaFull=%llu\n",
                g_mainWaitTicks * g_qpcToMs / frames, g_rtBusyTicks.load() * g_qpcToMs / frames,
                g_rtEndEfTicks.load() * g_qpcToMs / frames, g_commands / frames,
                static_cast<unsigned long long>(g_ringFullWaits), static_cast<unsigned long long>(g_arenaFullWaits));
        fprintf(f, "main-thread D3D calls while async: %llu, from other threads: %llu, sites:\n",
                static_cast<unsigned long long>(g_devCallsAsync), static_cast<unsigned long long>(g_devCallsOther));
        for (int i = 0; i < g_devSiteCount; i++)
        {
            const DevSite& d = g_devSites[i];
            HMODULE mod = nullptr;
            char name[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(d.ret), &mod);
            if (mod)
                GetModuleFileNameA(mod, name, sizeof(name));
            const char* base = strrchr(name, '\\');
            fprintf(f, "  dev method %3u x%-8llu %s+0x%llx\n", d.method, static_cast<unsigned long long>(d.count),
                    base ? base + 1 : name, static_cast<unsigned long long>(d.ret - reinterpret_cast<uintptr_t>(mod)));
        }
        fprintf(f, "sync slot  firstSync/frame  avgFirstMs  waits/frame  avgWaitMs\n");
        for (int i = 0; i < kMaxSyncReasons; i++)
        {
            const SyncStats& s = g_sync[i];
            if (!s.waits && !s.first)
                continue;
            fprintf(f, "%9d  %15.3f  %10.3f  %11.2f  %9.3f  %s\n", i, s.first / frames,
                    s.first ? s.firstTicks * g_qpcToMs / s.first : 0.0, s.waits / frames,
                    s.waits ? s.waitTicks * g_qpcToMs / s.waits : 0.0,
                    i >= kMethodReasonBase ? RendererMethodName(i - kMethodReasonBase) : "");
        }
    }
    fprintf(f, "query ids:");
    for (int i = 0; i < 256; i++)
        if (g_queryHist[i])
            fprintf(f, " %d=%.1f", i, g_queryHist[i] / frames);
    fprintf(f, "\nslot  frames%%  callsPerFrame  avgFirstCallMs\n");
    std::vector<int> order;
    for (int i = 0; i < g_slotCount; i++)
        if (g_slots[i].framesSeen)
            order.push_back(i);
    std::sort(order.begin(), order.end(), [](int a, int b) {
        return g_slots[a].firstSum * g_slots[b].framesSeen < g_slots[b].firstSum * g_slots[a].framesSeen;
    });
    for (int i : order)
    {
        const SlotStats& s = g_slots[i];
        fprintf(f, "%4d  %6.1f  %13.1f  %14.3f\n", i, 100.0 * s.framesSeen / frames, s.calls / frames,
                s.firstSum * g_qpcToMs / s.framesSeen);
    }
    WriteCensus(f);
    WriteFillSyncSites(f);
    WriteProbe(f);
    fclose(f);
}

// QPC time at which each top-level frame was dispatched, for matching slow frames with samples (CRYMT_FRAMELOG).
bool g_frameLogOn;
constexpr uint64_t kMaxFrameLog = 400000;
int64_t g_frameLog[kMaxFrameLog];

// Per-frame split of where main waits and what the render thread spends, for the first frames of a run.
// Main fields are indexed by the main frame, render-thread fields by the flush sequence number.
constexpr uint64_t kMaxDetail = 65536;
struct FrameDetail
{
    float waitEnd;     // WaitIdle(295): previous flush still running when main queues the frame
    float waitBarrier; // main held until preprocess and shadow prep are done
    float waitOther;   // other WaitIdle during the frame (fences, sync slots)
    uint32_t seq;
    int topReason;     // the WaitIdle reason main waited longest for in this frame
    float topWait;
};
FrameDetail g_detail[kMaxDetail];
float g_rtPre[kMaxDetail];   // flush start to engine callbacks done
float g_rtFlush[kMaxDetail]; // whole flush
float g_rtPhase[4][kMaxDetail]; // flush start to preprocess, preprocess, shadow prep, shadow map draws inside it
uint64_t g_waitAtFrameStart;
struct ReasonWait
{
    int reason;
    int64_t ticks;
};
ReasonWait g_frameWaits[8]; // this frame's WaitIdle time per reason (first 8 reasons)
int g_frameWaitCount;

void NoteFrameWait(int reason, int64_t ticks)
{
    for (int i = 0; i < g_frameWaitCount; i++)
        if (g_frameWaits[i].reason == reason)
        {
            g_frameWaits[i].ticks += ticks;
            return;
        }
    if (g_frameWaitCount < 8)
        g_frameWaits[g_frameWaitCount++] = {reason, ticks};
}

void WriteDetailLog()
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_detail.txt").c_str(), "w") != 0 || !f)
        return;
    fprintf(f, "frame waitEnd waitBarrier waitOther rtPre rtFlush seq topReason topWait rtStart rtPreproc rtShadow rtShadowDraw\n");
    const uint64_t n = std::min(g_frame, kMaxDetail);
    for (uint64_t i = 0; i < n; i++)
    {
        const FrameDetail& d = g_detail[i];
        const uint32_t s = d.seq % kMaxDetail;
        fprintf(f, "%llu %.3f %.3f %.3f %.3f %.3f %u %d %.3f %.3f %.3f %.3f %.3f\n", static_cast<unsigned long long>(i), d.waitEnd,
                d.waitBarrier, d.waitOther, g_rtPre[s], g_rtFlush[s], d.seq, d.topReason, d.topWait, g_rtPhase[0][s], g_rtPhase[1][s],
                g_rtPhase[2][s], g_rtPhase[3][s]);
    }
    fclose(f);
}

void WriteFrameLog()
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_frames.txt").c_str(), "w") != 0 || !f)
        return;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    fprintf(f, "qpcfreq %lld\n", static_cast<long long>(freq.QuadPart));
    const uint64_t n = g_frame < kMaxFrameLog ? g_frame : kMaxFrameLog;
    for (uint64_t i = 0; i < n; i++)
        fprintf(f, "%lld\n", static_cast<long long>(g_frameLog[i]));
    fclose(f);
}

void WriteTrace()
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_trace.txt").c_str(), "w") != 0 || !f)
        return;
    fprintf(f, "frame=%llu calls=%d\n", static_cast<unsigned long long>(g_traceFrame), g_traceCount);
    for (int i = 0; i < g_traceCount; i++)
    {
        const TraceEntry& e = g_trace[i];
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(e.ret), &mod);
        if (mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        fprintf(f, "%8.3f %4u %016llx %s+0x%llx\n", e.time * g_qpcToMs, e.slot, static_cast<unsigned long long>(e.arg),
                base ? base + 1 : name, static_cast<unsigned long long>(e.ret - reinterpret_cast<uintptr_t>(mod)));
    }
    fclose(f);
}
} // namespace

// The world frame counter (EndEf3D dispatches since start; the same frame of a timedemo in every run), for capture.
uint64_t CryMTWorldFrame() { return g_frame; }

// Shader file investigation (CRYMT_PAKLOG=1): ICryPak::FOpen through the renderer's gEnv (0x382E74D0, pCryPak at
// +0x80), vtable slot 17 FOpen(name, mode, flags) (the renderer calls *0x88 so) and slot 16 FOpen(name, mode, game path,
// length) (Mod SDK order, overloads reversed). Every open of a shader script, shader cache or the GI texture goes to
// logs\mt\crymt_paklog.txt with its result.
namespace
{
using PakOpenFlagsFn = FILE* (*)(void*, const char*, const char*, unsigned);
using PakOpenPathFn = FILE* (*)(void*, const char*, const char*, char*, int);
PakOpenFlagsFn g_pakOpenFlags;
PakOpenPathFn g_pakOpenPath;
FILE* g_pakLog;
CRITICAL_SECTION g_pakLogCs;
void* g_pak;

// Size of an open CryPak file (FSeek slot 22, FTell slot 23): tells which pak's copy it is.
long PakSize(FILE* f)
{
    void** vt = *reinterpret_cast<void***>(g_pak);
    const auto seek = reinterpret_cast<size_t (*)(void*, FILE*, long, int)>(vt[22]);
    const auto tell = reinterpret_cast<long (*)(void*, FILE*)>(vt[23]);
    seek(g_pak, f, 0, SEEK_END);
    const long size = tell(g_pak, f);
    seek(g_pak, f, 0, SEEK_SET);
    return size;
}

void PakLog(const char* how, const char* name, const char* mode, FILE* result)
{
    if (!name)
        return;
    char lower[260];
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(lower); n++)
        lower[n] = static_cast<char>(tolower(static_cast<unsigned char>(name[n])));
    lower[n] = 0;
    if (!strstr(lower, "shaders") && !strstr(lower, "crymt") && !strstr(lower, ".cfi") && !strstr(lower, ".cfx"))
        return;
    EnterCriticalSection(&g_pakLogCs);
    fprintf(g_pakLog, "%s %s [%s] -> %s %ld\n", how, name, mode ? mode : "", result ? "open" : "none", -1L);
    fflush(g_pakLog);
    LeaveCriticalSection(&g_pakLogCs);
}

FILE* PakOpenFlagsHook(void* pak, const char* name, const char* mode, unsigned flags)
{
    FILE* f = g_pakOpenFlags(pak, name, mode, flags);
    PakLog("open", name, mode, f);
    return f;
}

FILE* PakOpenPathHook(void* pak, const char* name, const char* mode, char* path, int length)
{
    FILE* f = g_pakOpenPath(pak, name, mode, path, length);
    PakLog("open-path", name, mode, f);
    return f;
}
} // namespace

void CryMTInstallPakLog()
{
    static bool done;
    if (done || !GetEnvironmentVariableW(L"CRYMT_PAKLOG", nullptr, 0))
        return;
    const auto* env = *reinterpret_cast<uint8_t* const*>(0x382E74D0);
    void* pak = env ? *reinterpret_cast<void* const*>(env + 0x80) : nullptr;
    if (!pak)
        return;
    done = true;
    g_pak = pak;
    InitializeCriticalSection(&g_pakLogCs);
    fopen_s(&g_pakLog, CryMTLog("crymt_paklog.txt").c_str(), "w");
    if (!g_pakLog)
        return;
    void** vt = *reinterpret_cast<void***>(pak);
    DWORD old;
    VirtualProtect(vt + 16, 2 * sizeof(void*), PAGE_READWRITE, &old);
    g_pakOpenPath = reinterpret_cast<PakOpenPathFn>(vt[16]);
    g_pakOpenFlags = reinterpret_cast<PakOpenFlagsFn>(vt[17]);
    vt[16] = reinterpret_cast<void*>(&PakOpenPathHook);
    vt[17] = reinterpret_cast<void*>(&PakOpenFlagsHook);
    VirtualProtect(vt + 16, 2 * sizeof(void*), old, &old);
    fprintf(g_pakLog, "CryPak %p vtable %p hooked\n", pak, static_cast<void*>(vt));
}

extern "C" void* DevGateC(uint32_t idx, uintptr_t ret)
{
    const DWORD tid = GetCurrentThreadId();
    if (tid != g_mainThread && tid != g_rtTid)
    {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_devCallsOther));
        return g_devOrig[idx];
    }
    if (g_async && tid == g_mainThread)
    {
        g_devCallsAsync++;
        int i = 0;
        while (i < g_devSiteCount && (g_devSites[i].method != idx || g_devSites[i].ret != ret))
            i++;
        if (i < g_devSiteCount)
            g_devSites[i].count++;
        else if (g_devSiteCount < kMaxDevSites)
            g_devSites[g_devSiteCount++] = {idx, ret, 1};
    }
    return g_devOrig[idx];
}

extern "C" void GateCommon();
extern "C" void DevGateCommon();
extern "C" uint64_t ReplayCall(void* target, CallFrame* regs, const uint64_t* stack);
asm(R"(
.intel_syntax noprefix
.text
.globl DevGateCommon
DevGateCommon:
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 0x68
    movups [rsp+0x20], xmm0
    movups [rsp+0x30], xmm1
    movups [rsp+0x40], xmm2
    movups [rsp+0x50], xmm3
    mov ecx, eax
    mov rdx, [rsp+0x88]
    call DevGateC
    movups xmm0, [rsp+0x20]
    movups xmm1, [rsp+0x30]
    movups xmm2, [rsp+0x40]
    movups xmm3, [rsp+0x50]
    add rsp, 0x68
    pop r9
    pop r8
    pop rdx
    pop rcx
    jmp rax

.globl GateCommon
GateCommon:
    push rbp
    mov rbp, rsp
    sub rsp, 0xA0
    mov [rsp+0x20], rcx
    mov [rsp+0x28], rdx
    mov [rsp+0x30], r8
    mov [rsp+0x38], r9
    movups [rsp+0x40], xmm0
    movups [rsp+0x50], xmm1
    movups [rsp+0x60], xmm2
    movups [rsp+0x70], xmm3
    mov ecx, eax
    lea rdx, [rsp+0x20]
    mov r8, rbp
    call GateC
    cmp rax, 1
    je 1f
    mov rcx, [rsp+0x20]
    mov rdx, [rsp+0x28]
    mov r8, [rsp+0x30]
    mov r9, [rsp+0x38]
    movups xmm0, [rsp+0x40]
    movups xmm1, [rsp+0x50]
    movups xmm2, [rsp+0x60]
    movups xmm3, [rsp+0x70]
    mov rsp, rbp
    pop rbp
    jmp rax
1:
    movups xmm0, [rsp+0x40]
    mov rax, [rsp+0x80]
    mov rsp, rbp
    pop rbp
    ret

.globl ReplayCall
ReplayCall:
    push rbp
    mov rbp, rsp
    push rbx
    push rsi
    sub rsp, 0xE0
    mov rbx, rcx
    mov rsi, rdx
    xor eax, eax
2:
    mov r10, [r8+rax*8]
    mov [rsp+0x20+rax*8], r10
    inc eax
    cmp eax, 24
    jb 2b
    mov rcx, [rsi]
    mov rdx, [rsi+0x08]
    mov r8, [rsi+0x10]
    mov r9, [rsi+0x18]
    movups xmm0, [rsi+0x20]
    movups xmm1, [rsi+0x30]
    movups xmm2, [rsi+0x40]
    movups xmm3, [rsi+0x50]
    call rbx
    mov [rsi+0x60], rax
    movups [rsi+0x20], xmm0
    add rsp, 0xE0
    pop rsi
    pop rbx
    pop rbp
    ret
.att_syntax prefix
)");

// Renderer interface census (CRYMT_CENSUS, with CRYMT_MODE=0): every method of every renderer class listed in
// logs\mt\census_vtables.txt (tools\prof\all_vtables.py) gets a stub that counts its calls, and the counts made
// between RenderWorld entry and the top-level EF_EndEf3D are summed: what main does to the renderer while it
// builds a frame. Stubs reach their counter and original through rip-relative operands in one allocation.
constexpr size_t kCensusStubSize = 16;
struct CensusData
{
    uint64_t count[kRendererMethodMax];
    uint64_t orig[kRendererMethodMax];
};
struct RendererClass
{
    uintptr_t vtable;
    int first, count;
    char name[72];
};
std::vector<RendererClass> g_rendererClasses;
int g_rendererMethods;
uint64_t g_methodOrig[kRendererMethodMax];
bool g_censusOn;
bool g_censusInR;
CensusData* g_census;
uint64_t g_censusStart[kRendererMethodMax];
uint64_t g_censusSum[kRendererMethodMax];
uint64_t g_censusFrames;

// Builds the class list once from the table compiled in (RendererClasses6156.h); methods are numbered in table
// order, CD3D9Renderer (gated) left out.
bool LoadRendererClasses()
{
    if (!g_rendererClasses.empty())
        return true;
    for (const RendererClassInfo& info : kRendererClassList)
    {
        const int n = static_cast<int>(info.count);
        if (info.vtable == kRendererVtable || g_rendererMethods + n > kRendererMethodMax)
            continue;
        RendererClass c{};
        c.vtable = info.vtable;
        c.first = g_rendererMethods;
        c.count = n;
        strcpy_s(c.name, info.name);
        for (int s = 0; s < n; s++)
            g_methodOrig[c.first + s] = reinterpret_cast<const uint64_t*>(c.vtable)[s];
        g_rendererMethods += n;
        g_rendererClasses.push_back(c);
    }
    return !g_rendererClasses.empty();
}

const char* RendererMethodName(int method)
{
    static char buf[96];
    for (const RendererClass& c : g_rendererClasses)
        if (method >= c.first && method < c.first + c.count)
        {
            sprintf_s(buf, "%s slot %d (%llx)", c.name, method - c.first,
                      static_cast<unsigned long long>(g_methodOrig[method] - kRenderBase));
            return buf;
        }
    return "?";
}

// Replaces vtable entry `method` with `stub`.
void PatchRendererMethod(int method, const void* stub)
{
    for (const RendererClass& c : g_rendererClasses)
        if (method >= c.first && method < c.first + c.count)
        {
            auto* entry = reinterpret_cast<uint64_t*>(c.vtable) + (method - c.first);
            DWORD old;
            VirtualProtect(entry, 8, PAGE_READWRITE, &old);
            *entry = reinterpret_cast<uint64_t>(stub);
            VirtualProtect(entry, 8, old, &old);
            return;
        }
}

void InstallCensus()
{
    if (!LoadRendererClasses())
        return;
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRendererMethodMax * kCensusStubSize + sizeof(CensusData),
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem)
        return;
    g_census = reinterpret_cast<CensusData*>(mem + kRendererMethodMax * kCensusStubSize);
    const auto rel = [](const uint8_t* next, const void* target) {
        return static_cast<int32_t>(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(next));
    };
    for (int idx = 0; idx < g_rendererMethods; idx++)
    {
        uint8_t* stub = mem + idx * kCensusStubSize;
        g_census->orig[idx] = g_methodOrig[idx];
        const int32_t countRel = rel(stub + 8, &g_census->count[idx]);
        const int32_t origRel = rel(stub + 14, &g_census->orig[idx]);
        stub[0] = 0xF0; stub[1] = 0x48; stub[2] = 0xFF; stub[3] = 0x05; // lock inc qword ptr [rip+count]
        memcpy(stub + 4, &countRel, 4);
        stub[8] = 0xFF; stub[9] = 0x25; // jmp qword ptr [rip+orig]
        memcpy(stub + 10, &origRel, 4);
        PatchRendererMethod(idx, stub);
    }
    FlushInstructionCache(GetCurrentProcess(), mem, kRendererMethodMax * kCensusStubSize);
    char line[96];
    sprintf_s(line, "census: %d classes, %d methods", static_cast<int>(g_rendererClasses.size()), g_rendererMethods);
    SetupLog(line);
}

void CensusBegin()
{
    if (!g_census)
        return;
    memcpy(g_censusStart, g_census->count, sizeof(uint64_t) * g_rendererMethods);
    g_censusInR = true;
}

void CensusEnd()
{
    if (!g_census || !g_censusInR)
        return;
    for (int i = 0; i < g_rendererMethods; i++)
        g_censusSum[i] += g_census->count[i] - g_censusStart[i];
    g_censusFrames++;
    g_censusInR = false;
}

void WriteCensus(FILE* f)
{
    if (!g_census || !g_censusFrames)
        return;
    fprintf(f, "census over %llu frames: renderer methods main calls during RenderWorld (calls/frame, method)\n",
            static_cast<unsigned long long>(g_censusFrames));
    std::vector<std::pair<uint64_t, int>> order;
    for (int i = 0; i < g_rendererMethods; i++)
        if (g_censusSum[i])
            order.push_back({g_censusSum[i], i});
    std::sort(order.rbegin(), order.rend());
    for (const auto& [sum, i] : order)
        fprintf(f, "  %10.2f  %s\n", static_cast<double>(sum) / g_censusFrames, RendererMethodName(i));
}

// Fill window (CRYMT_FILL, mode 2): from RenderWorld entry to the top-level EF_EndEf3D main builds the next
// frame while the render thread may still draw the previous one. Every method of every other renderer class is
// wrapped: inside the window on main, a call not known to be safe first waits for the render thread (a sync
// point). Overlap is then always safe, and it grows as calls are converted to deferred or read-only ones;
// the sync slot table shows which method stops main first and when.
bool g_fillOn;
bool g_fillWindow;

// Read-only getters of mesh, shader and resource fields (decompiled: they only load fields).
struct PassMethod
{
    const char* cls;
    int slot;
};
constexpr PassMethod kPassMethods[] = {
    // 0x380905E0: the largest visible sample count over the GPUs (RE +0x50), written when the render thread reads the
    // queries; the ocean (3D+0xC615B) takes the last result, which is a frame old anyway.
    {".?AVCREOcclusionQuery@@", 25},
    // 0x3812A930: the ocean's FFT displacement grid (64 x 64, 16 bytes each, allocated once) that the render thread
    // updates in place; COcean::GetWaterLevel (3D+0xC3F40) samples heights from it for buoyancy and effects.
    {".?AVCREWaterOcean@@", 24},
    {".?AVCShader@@", 5},
    {".?AVCShader@@", 6},      {".?AVCShader@@", 7},      {".?AVCShader@@", 10},     {".?AVCShader@@", 15},
    {".?AVCShader@@", 16},     {".?AUSRenderShaderResources@@", 12}, {".?AUSRenderShaderResources@@", 19},
};

// FUN_3809DDF0 (view-projection update, reached from SetCamera) ends with a tail jump to FUN_3803A390, which
// sets bits 0x202000 at +0xAF8 of the global renderer, not of `this`. A mirrored call on the fake skips it; the
// queued real call sets them on the render thread.

extern "C" void MatricesDirtyHook()
{
    if (g_mirroring && GetCurrentThreadId() == g_mainThread)
        return;
    reinterpret_cast<void (*)()>(kMatricesDirtyFn)();
}

// Callers of the sync points made by wrapped methods, to find the engine code behind them.
struct FillSyncSite
{
    int method; // wrapped method, or -1 - slot for a renderer call
    uintptr_t ret;
    uint64_t count;
    uint64_t arg1, arg2; // of the last call
};
FillSyncSite g_fillSyncSites[32];

void NoteFillSync(int method, uintptr_t ret, uint64_t arg1, uint64_t arg2)
{
    for (FillSyncSite& s : g_fillSyncSites)
        if (!s.count || (s.method == method && s.ret == ret))
        {
            s = {method, ret, s.count + 1, arg1, arg2};
            break;
        }
}

// CRenderMesh slots 9 and 11 (0x381580D0, 0x381581E0) return stream pointers; with the fourth argument false
// they only read the system copy (the occlusion culler, 3D+0x138012), with true they lock and mark the stream.
int g_meshFirstMethod = -1; // CRenderMesh slot 0 in the method numbering
bool IsMeshMethod(int method, MeshSlot slot)
{
    return g_meshFirstMethod >= 0 && method == g_meshFirstMethod + slot;
}

// CRenderMesh methods that only read fields (decompiled): 8 GetIndices (+0xE8, count +0xF0), 18 the chunk array
// (+0x108), 22 the index count, 7/17/19/45 other fields, 59/60 triangle areas and a random point from the system
// copies (geometry particle emitters call them holding the 3D engine lock the flush needs, 3D+0x319B0), and the
// rest of the old pass list. They run without a sync unless the mesh has queued edits.
constexpr int kMeshReaders[] = {7, 8, 17, 18, 19, 22, 35, 37, 39, 41, 45, 46, 59, 60}; // 46: vertex format (+0xFC)

bool IsMeshReader(int method)
{
    for (int slot : kMeshReaders)
        if (method == g_meshFirstMethod + slot)
            return true;
    return false;
}

// CREOcclusionQuery slot 23 (0x380912A0, mfReadResult_Try: GetData on the D3D query, stores the samples at +0x50,
// returns whether the result was there). The ocean (3D+0xC624C) issues a new query when it returns true. Queued; main
// gets what the render thread's last call for that element returned, so results arrive a frame later.
int g_occlusionReadMethod = -1;
struct QueryReady
{
    uintptr_t re;
    std::atomic<uint8_t> ready;
};
QueryReady g_queryReady[64];
int g_queryReadyCount;

// Meshes with queued edits the render thread has not run yet (cleared by WaitIdle).
constexpr size_t kPendingMeshMax = 256;
uintptr_t g_pendingMeshes[kPendingMeshMax];
size_t g_pendingMeshCount;

bool MeshEditPending(uintptr_t mesh)
{
    for (size_t i = 0; i < g_pendingMeshCount; i++)
        if (g_pendingMeshes[i] == mesh)
            return true;
    return false;
}

bool QueueMeshEdit(int method, const uint64_t* frame);
int QueueOcclusionRead(int method, const uint64_t* frame);
void QueueResourceAddRef(int method, const uint64_t* frame);

// frame: the thunk's saved r9, r8, rdx, rcx, then the return address, the home area and the stack arguments.
// Returns nonzero when the call was queued: the thunk then returns that value minus one without calling the method.
extern "C" int __cdecl FillMethodHook(int method, uintptr_t ret, uint64_t* frame)
{
    if (GetCurrentThreadId() != g_mainThread)
        return 0;
    if (g_mirroring)
    {
        g_mirrorMethodCalls++;
        return 0;
    }
    if (method == g_occlusionReadMethod && g_async)
        if (const int r = QueueOcclusionRead(method, frame))
            return r;
    // Resource AddRef (shaders, textures): C3DEngine::AddDynamicLightSource (3D+0xFD634/0xFD645) takes a reference to each
    // dynamic light's shader and projected texture; on harbor that was ~0.7 syncs a frame of ~9 ms each. The count is
    // not atomic and the render thread releases these resources, so the increment is queued in order with its work.
    // Release, GetRefCounter and every other method of the resource still sync, so they see the queued increment;
    // only AddDynamicLightSource's own releases are queued too (it ignores the result, while the renderer's
    // release-until-zero loop 0x38178D20 relies on it).
    const uint64_t target = g_methodOrig[method];
    if (target == kResourceAddRefFn && g_async)
    {
        QueueResourceAddRef(method, frame);
        return 2; // the thunk returns 1; callers ignore the old count
    }
    if ((target == kShaderReleaseFn || target == kTextureReleaseFn) && g_async && ret - kAddDynamicLightFn < kAddDynamicLightSize)
    {
        QueueResourceAddRef(method, frame);
        g_resourceReleasesQueued++;
        return 2;
    }
    // Texture flags (+0xB8): the light code tests bit 9 of each projected texture, ~96 times a frame on harbor.
    if (target == kTextureGetFlagsFn)
        return 0;
    const bool meshMethod = g_meshFirstMethod >= 0 && method >= g_meshFirstMethod && method < g_meshFirstMethod + kRenderMeshSlots;
    // Meshes main created in this fill window (the render thread has not seen them) and the one being created.
    if (meshMethod && (g_inMeshCreate || (!IsMeshMethod(method, kMeshRelease) && !g_mainMeshes.empty() && IsMainMesh(frame[3]))))
    {
        g_mainMeshCalls++;
        return 0;
    }
    // Mesh edits (SetIndices with the indices copied, SetChunk) are queued. Until the render thread has run them,
    // every other method of that mesh syncs: the shortcuts below read the fields they change.
    // SetMaterial (by value, the material AddRef'd by the callee) joins them on a mesh that already has queued edits;
    // SetBBox with a new box (the ocean every frame) is queued with the two vectors copied.
    const bool sameBox = IsMeshMethod(method, kMeshSetBBox) &&
                         memcmp(reinterpret_cast<const void*>(frame[3] + kMeshBBoxMin), reinterpret_cast<const void*>(frame[2]), 12) == 0 &&
                         memcmp(reinterpret_cast<const void*>(frame[3] + kMeshBBoxMax), reinterpret_cast<const void*>(frame[1]), 12) == 0;
    if ((IsMeshMethod(method, kMeshSetIndices) || IsMeshMethod(method, kMeshSetChunk) || IsMeshMethod(method, kMeshSetVertexContainer) ||
         IsMeshMethod(method, kMeshSetB0) || (IsMeshMethod(method, kMeshSetMaterial) && MeshEditPending(frame[3])) ||
         (IsMeshMethod(method, kMeshSetBBox) && !sameBox)) &&
        g_async && QueueMeshEdit(method, frame))
        return 1;
    if (meshMethod && MeshEditPending(frame[3]))
    {
        if (g_async)
            NoteFillSync(method, ret, frame[2], frame[1]);
        WaitIdlePublic(kMethodReasonBase + method);
        return 0;
    }
    if (meshMethod && IsMeshReader(method))
        return 0;
    if ((IsMeshMethod(method, kMeshStreamPtr) || IsMeshMethod(method, kMeshStreamPtr2)) && (frame[0] & 0xFF) == 0)
        return 0;
    // Slot 36 (SetBBox(min, max), six floats at +0x130) with the box the mesh has (terrain sets it every frame).
    if (sameBox)
        return 0;
    // CRenderMesh slot 34 (0x38157DF0: material at +0x30 with AddRef/Release, chunk values when the third argument
    // is nonzero) setting the material the mesh already has changes nothing.
    // Slot 25 (0x38157E80) sets every chunk's render element +0x10. Chunks: TArray at mesh+0x108, 0x48 bytes each,
    // render element at +0x20, value at +0x18.
    const auto chunksAll = [&](size_t fieldInChunk, size_t fieldInRe, uint64_t value, bool viaRe) {
        const auto* arr = *reinterpret_cast<const uint8_t* const*>(frame[3] + kMeshChunks);
        if (!arr)
            return true;
        const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(arr);
        const int n = *reinterpret_cast<const int*>(arr + 8);
        for (int k = 0; k < n; k++)
        {
            const uint8_t* chunk = data + k * kMeshChunkBytes;
            if (viaRe)
            {
                const uint8_t* re = *reinterpret_cast<const uint8_t* const*>(chunk + 0x20);
                if (re && *reinterpret_cast<const uint64_t*>(re + fieldInRe) != value)
                    return false;
            }
            else if (*reinterpret_cast<const uint8_t* const*>(chunk + 0x20) &&
                     *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(chunk + 0x20) + fieldInChunk) != static_cast<uint32_t>(value))
                return false;
        }
        return true;
    };
    // With the material the mesh has, SetMaterial only stores the chunk value into the chunks' render elements (+0x18).
    // The terrain (3D+0xBF050) then writes the same fields directly; when its ids change, TerrainTexturesHook queues
    // both, so what reaches here sets the values the elements have.
    if (IsMeshMethod(method, kMeshSetMaterial) && frame[2] == *reinterpret_cast<const uint64_t*>(frame[3] + kMeshMaterial))
        return 0;
    if (IsMeshMethod(method, kMeshSetChunkRe) && chunksAll(0, 0x10, frame[2], true))
        return 0;
    if (g_async)
        NoteFillSync(method, ret, frame[2], frame[1]);
    WaitIdlePublic(kMethodReasonBase + method);
    // Registers only: the stack slots past a method's own arguments are its caller's locals. The methods that
    // take objects on the stack (CRenderMesh::Render, AddRenderElements) have their own hooks (InstallProxyHooks).
    if (g_translateProxies)
        g_translateProxies(frame, 4, frame + 9, 0);
    return 0;
}

void WriteFillSyncSites(FILE* f)
{
    for (const FillSyncSite& s : g_fillSyncSites)
    {
        if (!s.count)
            break;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(s.ret), &mod);
        if (mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        char slot[32];
        snprintf(slot, sizeof(slot), "renderer slot %d", -1 - s.method);
        fprintf(f, "  fill sync x%-8llu %s from %s+0x%llx (last args %llx %llx)\n", static_cast<unsigned long long>(s.count),
                s.method < 0 ? slot : RendererMethodName(s.method), base ? base + 1 : name,
                static_cast<unsigned long long>(s.ret - reinterpret_cast<uintptr_t>(mod)), static_cast<unsigned long long>(s.arg1),
                static_cast<unsigned long long>(s.arg2));
    }
}

// "if (g_fillWindow && (r = FillMethodHook(method))) return r - 1; jmp original" with the argument registers kept.
uint8_t* EmitFillThunk(uint8_t* c, uint64_t original, int method)
{
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes)
            *c++ = b;
    };
    auto put64 = [&](uint64_t v) {
        memcpy(c, &v, 8);
        c += 8;
    };
    put({0x48, 0xB8});                               // movabs rax, &g_fillWindow
    put64(reinterpret_cast<uint64_t>(&g_fillWindow));
    put({0x80, 0x38, 0x00});                         // cmp byte ptr [rax], 0
    put({0x75, 0x0C});                               // jne slow
    put({0x48, 0xB8});                               // movabs rax, original
    put64(original);
    put({0xFF, 0xE0});                               // jmp rax
    put({0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x68});  // slow: push rcx,rdx,r8,r9; sub rsp,0x68
    put({0x0F, 0x11, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x4C, 0x24, 0x30}); // movups [rsp+0x20],xmm0; [rsp+0x30],xmm1
    put({0x0F, 0x11, 0x54, 0x24, 0x40, 0x0F, 0x11, 0x5C, 0x24, 0x50}); // movups [rsp+0x40],xmm2; [rsp+0x50],xmm3
    put({0xB9});                                     // mov ecx, method
    memcpy(c, &method, 4);
    c += 4;
    put({0x48, 0x8B, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00}); // mov rdx, [rsp+0x88]: the method's return address
    put({0x4C, 0x8D, 0x44, 0x24, 0x68});                   // lea r8, [rsp+0x68]: saved r9, r8, rdx, rcx, ret, home, stack args
    put({0x48, 0xB8});                               // movabs rax, FillMethodHook
    put64(reinterpret_cast<uint64_t>(&FillMethodHook));
    put({0xFF, 0xD0});                               // call rax
    put({0x0F, 0x10, 0x44, 0x24, 0x20, 0x0F, 0x10, 0x4C, 0x24, 0x30});
    put({0x0F, 0x10, 0x54, 0x24, 0x40, 0x0F, 0x10, 0x5C, 0x24, 0x50});
    put({0x48, 0x83, 0xC4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59}); // add rsp,0x68; pop r9,r8,rdx,rcx
    put({0x85, 0xC0, 0x74, 0x03, 0xFF, 0xC8, 0xC3}); // test eax,eax; jz +3; dec eax; ret (queued: returns hook value - 1)
    put({0x48, 0xB8});                               // movabs rax, original
    put64(original);
    put({0xFF, 0xE0});                               // jmp rax
    return c;
}

void InstallMeshListHooks();
void InstallShadowRecording();

void InstallFillHooks()
{
    if (!LoadRendererClasses())
        return;
    InstallMeshListHooks();
    InstallShadowRecording();
    constexpr size_t kThunkMax = 160; // EmitFillThunk writes 136 bytes
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_rendererMethods * kThunkMax, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem)
        return;
    int wrapped = 0;
    for (const RendererClass& cls : g_rendererClasses)
    {
        if (strcmp(cls.name, ".?AUCRenderMesh@@") == 0)
            g_meshFirstMethod = cls.first;
        if (strcmp(cls.name, ".?AVCREOcclusionQuery@@") == 0)
            g_occlusionReadMethod = cls.first + 23;
    }
    for (const RendererClass& cls : g_rendererClasses)
        for (int s = 0; s < cls.count && cls.vtable != kAuxVtable; s++)
        {
            bool pass = false;
            for (const PassMethod& p : kPassMethods)
                pass = pass || (p.slot == s && strcmp(p.cls, cls.name) == 0);
            if (pass)
                continue;
            const int method = cls.first + s;
            uint8_t* thunk = mem + method * kThunkMax;
            EmitFillThunk(thunk, g_methodOrig[method], method);
            PatchRendererMethod(method, thunk);
            wrapped++;
        }
    DWORD old;
    VirtualProtect(mem, g_rendererMethods * kThunkMax, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, g_rendererMethods * kThunkMax);
    char line[96];
    sprintf_s(line, "fill window: %d renderer methods wrapped", wrapped);
    SetupLog(line);
}

// GPU time per pass (CRYMT_GPUPROF): D3D10 timestamp queries on the render thread around every batch list
// of the flush (FX_ProcessBatchesList, FUN_380A4480), so the gaps between lists hold shadow maps and post
// effects; then the 2D/HUD commands after the flush and EndFrame. Results are read back frames later.
constexpr uint8_t kBatchListPrologue[] = {0x44, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uintptr_t kBatchListOffset = 0xF04; // list id stored by FX_ProcessRenderList (FUN_380A5390)
constexpr int kGpuInFlight = 8;
constexpr int kGpuMarks = 160;
constexpr int kGpuLabels = 64;
constexpr uint32_t kGpuFrames = 8192;
enum GpuKind : uint32_t
{
    kGpuList = 1,
    kGpuAfterList = 2,
    kGpuFlushStart = 3,
    kGpuAfterFlush = 4,
    kGpuEndFrame = 5,
    kGpuDone = 6,
    kGpuInner = 7,
};

// Marks inside EndFrame, placed in front of its direct calls, split its GPU time.
struct GpuInnerSite
{
    uintptr_t site, target;
    const char* name;
};
constexpr GpuInnerSite kGpuInnerSites[] = {
    {0x380E5C0E, 0x3813A900, "ef.texMgr"},  {0x380E6546, 0x3808D290, "ef.8D290"},   {0x380E6869, 0x38035CB0, "ef.debugMrt"},
    {0x380E68A9, 0x380DB850, "ef.capture"}, {0x380E68BC, 0x381BE740, "ef.1BE740"},  {0x380E68FC, 0x381BE590, "ef.1BE590a"},
    {0x380E6922, 0x381C21E0, "ef.1C21E0"},  {0x380E6950, 0x381C4BE0, "ef.1C4BE0"},  {0x380E6A03, 0x381BE590, "ef.1BE590b"},
    {0x380E6A9B, 0x380CF390, "ef.CF390"},   {0x380E6B37, 0x3807BA60, "ef.7BA60a"},  {0x380E6B89, 0x3807BA60, "ef.7BA60b"},
};

struct GpuFrame
{
    ID3D10Query* disjoint;
    ID3D10Query* ts[kGpuMarks];
    int64_t cpu[kGpuMarks];
    uint8_t label[kGpuMarks];
    int marks;
    uint32_t seq;
    bool pending;
};
bool g_gpuProf;
ID3D10Device* g_gpuDev;
GpuFrame g_gpu[kGpuInFlight];
GpuFrame* g_gpuCur;
uint32_t g_gpuLabelKey[kGpuLabels];
int g_gpuLabelCount;
float g_gpuMs[kGpuFrames][kGpuLabels];
float g_cpuMs[kGpuFrames][kGpuLabels]; // render-thread time between the same marks
uint32_t g_gpuSeq[kGpuFrames];
uint64_t g_gpuDropped;
using BatchListFn = void(__fastcall*)(uintptr_t, int, int, uint32_t);
BatchListFn g_batchList;

int GpuLabel(uint32_t key)
{
    for (int i = 0; i < g_gpuLabelCount; i++)
        if (g_gpuLabelKey[i] == key)
            return i;
    if (g_gpuLabelCount == kGpuLabels)
        return -1;
    g_gpuLabelKey[g_gpuLabelCount] = key;
    return g_gpuLabelCount++;
}

void GpuMark(uint32_t key)
{
    GpuFrame* f = g_gpuCur;
    if (!f || f->marks == kGpuMarks)
        return;
    if (!f->ts[f->marks])
    {
        const D3D10_QUERY_DESC desc{D3D10_QUERY_TIMESTAMP, 0};
        if (FAILED(g_gpuDev->CreateQuery(&desc, &f->ts[f->marks])))
            return;
    }
    const int label = GpuLabel(key);
    if (label < 0)
        return;
    f->ts[f->marks]->End();
    f->cpu[f->marks] = Now();
    f->label[f->marks++] = static_cast<uint8_t>(label);
}

// Each interval between two marks is charged to the label of the first one.
void GpuCollect()
{
    for (GpuFrame& f : g_gpu)
    {
        if (!f.pending)
            continue;
        D3D10_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (f.disjoint->GetData(&dj, sizeof(dj), D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        uint64_t t[kGpuMarks];
        bool ok = !dj.Disjoint && dj.Frequency != 0;
        for (int i = 0; i < f.marks && ok; i++)
            ok = f.ts[i]->GetData(&t[i], sizeof(t[i]), D3D10_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ok)
        {
            g_gpuDropped++;
            f.pending = false;
            continue;
        }
        float* row = g_gpuMs[f.seq % kGpuFrames];
        float* cpuRow = g_cpuMs[f.seq % kGpuFrames];
        memset(row, 0, sizeof(g_gpuMs[0]));
        memset(cpuRow, 0, sizeof(g_cpuMs[0]));
        for (int i = 0; i + 1 < f.marks; i++)
        {
            row[f.label[i]] += static_cast<float>((t[i + 1] - t[i]) * 1000.0 / dj.Frequency);
            cpuRow[f.label[i]] += static_cast<float>((f.cpu[i + 1] - f.cpu[i]) * g_qpcToMs);
        }
        g_gpuSeq[f.seq % kGpuFrames] = f.seq;
        f.pending = false;
    }
}

void GpuFrameEnd()
{
    GpuFrame* f = g_gpuCur;
    if (!f)
        return;
    GpuMark(kGpuDone << 28);
    f->disjoint->End();
    f->pending = true;
    g_gpuCur = nullptr;
}


void LogSwapChain(uintptr_t renderer)
{
    auto* unk = *reinterpret_cast<IUnknown**>(renderer + kSwapChainOffset);
    IDXGISwapChain* sc = nullptr;
    if (!unk || FAILED(unk->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc))))
    {
        SetupLog("swap chain: not found");
        return;
    }
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    UINT latency = 0;
    IDXGIDevice1* dxgiDev = nullptr;
    if (SUCCEEDED(g_gpuDev->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDev))))
    {
        dxgiDev->GetMaximumFrameLatency(&latency);
        dxgiDev->Release();
    }
    char line[200];
    sprintf_s(line, "swap chain: %ux%u fmt=%d buffers=%u effect=%d windowed=%d flags=%x refresh=%u/%u msaa=%u latency=%u",
              d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.Format, d.BufferCount, d.SwapEffect, d.Windowed, d.Flags,
              d.BufferDesc.RefreshRate.Numerator, d.BufferDesc.RefreshRate.Denominator, d.SampleDesc.Count, latency);
    SetupLog(line);
    sc->Release();
    // D3D10 runs on the D3D11 runtime (d3d11.dll); a D3D11 view of the device would let D3D11 libraries (upscalers)
    // work on the game's resources.
    ID3D11Device* d11 = nullptr;
    const HRESULT hr = g_gpuDev->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&d11));
    sprintf_s(line, "D3D11 interface of the D3D10 device: hr=%08lx feature level=%x", static_cast<unsigned long>(hr),
              d11 ? static_cast<unsigned>(d11->GetFeatureLevel()) : 0u);
    SetupLog(line);
    if (d11)
        d11->Release();
}

void GpuFrameBegin(uintptr_t renderer, uint32_t seq)
{
    if (!g_gpuProf)
        return;
    if (!g_gpuDev)
    {
        auto* dev = *reinterpret_cast<IUnknown**>(renderer + kDeviceOffset);
        if (!dev || FAILED(dev->QueryInterface(__uuidof(ID3D10Device), reinterpret_cast<void**>(&g_gpuDev))))
        {
            g_gpuProf = false;
            return;
        }
        LogSwapChain(renderer);
    }
    GpuFrameEnd();
    GpuCollect();
    for (GpuFrame& f : g_gpu)
        if (!f.pending)
        {
            if (!f.disjoint)
            {
                const D3D10_QUERY_DESC desc{D3D10_QUERY_TIMESTAMP_DISJOINT, 0};
                if (FAILED(g_gpuDev->CreateQuery(&desc, &f.disjoint)))
                    return;
            }
            f.marks = 0;
            f.seq = seq;
            f.disjoint->Begin();
            g_gpuCur = &f;
            GpuMark(kGpuFlushStart << 28);
            return;
        }
    g_gpuDropped++;
}

extern "C" void __cdecl GpuInnerHook(int site)
{
    if (g_gpuCur && GetCurrentThreadId() == g_rtTid)
        GpuMark(kGpuInner << 28 | static_cast<uint32_t>(site));
}

// ---------------------------------------------------------------------------------------------------------------------
// DLSS (CRYMT_DLSS=1 with the D3D12 back end): sub-pixel camera jitter in SetCamera, and the scene handed to the back
// end before HDR post-processing, so tone mapping, bloom, motion blur and the HUD work on the anti-aliased image.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
bool g_dlssOn;
bool g_batchListHooked;
uint32_t g_dlssFrame = 1;     // render thread: scenes handed over, the jitter phase
uint32_t g_dlssSceneFrame;    // the frame whose scene was handed over
uint64_t g_dlssScenes;

struct CameraSite
{
    uintptr_t caller;
    int level;
    bool real;
    int width, height;
    float fov, aspect, nearZ, farZ;
    uint64_t count;
};
CameraSite g_cameraSites[32];
SRWLOCK g_cameraSitesLock = SRWLOCK_INIT;

void NoteCamera(uintptr_t caller, int level, bool real, const uint8_t* camera)
{
    const int width = *reinterpret_cast<const int*>(camera + kCamWidth);
    const int height = *reinterpret_cast<const int*>(camera + kCamHeight);
    AcquireSRWLockExclusive(&g_cameraSitesLock);
    for (CameraSite& s : g_cameraSites)
        if (!s.count || (s.caller == caller && s.level == level && s.real == real && s.width == width && s.height == height))
        {
            s = {caller, level, real, width, height, *reinterpret_cast<const float*>(camera + kCamFov), *reinterpret_cast<const float*>(camera + kCamAspect),
                 *reinterpret_cast<const float*>(camera + kCamNear), *reinterpret_cast<const float*>(camera + kCamFar), s.count + 1};
            break;
        }
    ReleaseSRWLockExclusive(&g_cameraSitesLock);
}

// Jitter: a Halton (2, 3) sequence of 16 sub-pixel offsets, one per scene, added to the projection of every camera that
// views the screen (the main camera and the first-person weapon's near camera; shadow frustums have other shapes).
// DLSS takes the image's displacement in pixels (+x right, +y down), the convention Streamline also hands to DirectSR
// (as FSR2: translate clip space by (2 jx / W, -2 jy / H) and pass (jx, jy)). For the right-handed projection (w = -z)
// m20 -= 2 jx / W moves the image jx pixels right and m21 += 2 jy / H moves it jy pixels down. Static scenes cannot
// tell the sign apart (any accumulation of jittered frames converges to their average); CRYMT_DLSS_JITTER=-1 flips the
// value given to DLSS, "seq:..." runs comparison segments.
DlssSceneInfo g_dlssScene; // render thread: the main camera of the scene being rendered

// A lamp for the GI's voxel light from a CDLight (DlssSceneInfo::lamps layout): false for the sun, fake lights, others.
bool LampData(const uint8_t* l, float* data)
{
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(l + kLightFlags);
    const float radius = *reinterpret_cast<const float*>(l + kLightRadius);
    const float* o = reinterpret_cast<const float*>(l + kLightOrigin);
    const float* c = reinterpret_cast<const float*>(l + kLightColour);
    if ((flags & (kLightFlagSun | kLightFlagsFake)) || !(flags & (kLightFlagPoint | kLightFlagProjector)) || !(radius > 0.1f && radius < 200.0f) ||
        !(c[0] + c[1] + c[2] > 0.001f))
        return false;
    float dir[3] = {0, 0, -1}, cosHalf = -2;
    if (flags & kLightFlagProjector)
    {
        const float* m = reinterpret_cast<const float*>(l + kLightMatrix); // Matrix34, rows
        const float len = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
        if (len > 1e-4f)
        {
            dir[0] = m[0] / len;
            dir[1] = m[4] / len;
            dir[2] = m[8] / len;
            cosHalf = cosf(*reinterpret_cast<const float*>(l + kLightProjectorFov) * 0.5f * 3.14159265f / 180.0f);
        }
    }
    const float lamp[12] = {o[0], o[1], o[2], radius, c[0], c[1], c[2], cosHalf, dir[0], dir[1], dir[2], 0};
    memcpy(data, lamp, sizeof(lamp));
    return true;
}

// Every lamp of the level by value (main thread at PreUpdate, before the game update may delete a light entity; the
// render thread picks from the copy): the light entities of C3DEngine + 0x788 but the sun's. Lamps a portal or the view
// culls keep lighting the voxels, broken ones (deleted entities) stop. -1: none read (the frame's lights instead).
constexpr int kLampSnapMax = 128;
constexpr int kVisSnapMax = 512;
float g_visSnap[kVisSnapMax][40]; // the level's VisAreas (DlssSceneInfo::visAreas layout), under g_lampLock
int g_visSnapCount;
SRWLOCK g_lampLock = SRWLOCK_INIT;
float g_lampSnap[kLampSnapMax][12];
int g_lampSnapCount = -1;

void SnapshotLamps()
{
    float local[kLampSnapMax][12];
    int n = -1;
    const auto* engine = *reinterpret_cast<const uint8_t* const*>(k3DEngineGlobal);
    if (engine)
    {
        const auto* const* entities = *reinterpret_cast<const uint8_t* const* const*>(engine + kEngineLightEntities);
        const int count = *reinterpret_cast<const int*>(engine + kEngineLightEntities + 8);
        const void* sun = *reinterpret_cast<void* const*>(engine + kEngineSunEntity);
        if (entities && count >= 0 && count < 100000)
        {
            n = 0;
            for (int i = 0; i < count && n < kLampSnapMax; i++)
                if (entities[i] && entities[i] != sun && LampData(entities[i] + kLightEntityLight, local[n]))
                    n++;
        }
    }
    // The VisAreas (static once a level is loaded; read here with the lamps): box, height range and outline.
    static float areas[kVisSnapMax][40];
    int na = 0;
    const auto* mgr = *reinterpret_cast<const uint8_t* const*>(kVisAreaManagerGlobal);
    if (mgr)
    {
        const uint8_t* const* list = *reinterpret_cast<const uint8_t* const* const*>(mgr + kVamVisAreas);
        const int count = *reinterpret_cast<const int*>(mgr + kVamVisAreas + 8);
        for (int i = 0; list && count >= 0 && count < 100000 && i < count && na < kVisSnapMax; i++)
        {
            const uint8_t* a = list[i];
            const float* box = reinterpret_cast<const float*>(a + kVisAreaBox);
            const float* pts = *reinterpret_cast<const float* const*>(a + kVisAreaPoints);
            const int np = std::min(*reinterpret_cast<const int*>(a + kVisAreaPoints + 8), 16);
            if (!a || a[0x17D] || !pts || np < 3)
                continue; // portals are not rooms
            float* d = areas[na++];
            memset(d, 0, sizeof(areas[0]));
            const float head[8] = {box[0], box[1], box[3], box[4], box[2], box[5], static_cast<float>(np), 0};
            memcpy(d, head, sizeof(head));
            for (int k = 0; k < np; k++)
            {
                d[8 + k * 2] = pts[k * 3];
                d[9 + k * 2] = pts[k * 3 + 1];
            }
        }
    }
    AcquireSRWLockExclusive(&g_lampLock);
    if (n > 0)
        memcpy(g_lampSnap, local, static_cast<size_t>(n) * sizeof(local[0]));
    g_lampSnapCount = n;
    memcpy(g_visSnap, areas, static_cast<size_t>(na) * sizeof(areas[0]));
    g_visSnapCount = na;
    ReleaseSRWLockExclusive(&g_lampLock);
}
float g_jitterX, g_jitterY;
float g_jitterSign = 1.0f;
uint64_t g_jitteredCameras[2], g_suspendedCameras; // main / weapon cameras given the jitter; cameras skipped while suspended
bool g_jitterSuspended; // after the scene is handed over, until the next frame's first camera
bool g_topJittered;     // the projection on top of the stack carries this frame's jitter
bool g_dlssReset = true;
DWORD g_jitterSwitchTick;
// CRYMT_DLSS_JITTER="seq:<start s>:<step s>:<mode>,<mode>,..." for image comparisons: each mode is two signs (x, y)
// for the value given to DLSS, or R for a reference segment (jitter without DLSS). Times are from process start.
struct JitterMode
{
    float sx, sy;
    bool reference;
};
float g_jitterSignY = 1.0f;
std::vector<JitterMode> g_jitterModes;
DWORD g_jitterModeStart, g_jitterModeStep;
size_t g_jitterMode = SIZE_MAX;
bool g_jitterReference;

DWORD ProcessMilliseconds()
{
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<DWORD>((ticks(now) - ticks(created)) / 10000);
}

float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index; index /= base)
    {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
    }
    return r;
}

bool g_jitterOff; // CRYMT_DLSS_JITTER=0 (tests): DLSS without the camera jitter (temporal filter only)

void NextJitter()
{
    const uint32_t phase = g_dlssFrame % 16 + 1;
    g_jitterX = g_jitterOff ? 0.0f : Halton(phase, 2) - 0.5f;
    g_jitterY = g_jitterOff ? 0.0f : Halton(phase, 3) - 0.5f;
}

void ApplyJitter(float* proj, uint32_t width, uint32_t height, float scale)
{
    proj[8] -= scale * 2.0f * g_jitterX / static_cast<float>(width);
    proj[9] += scale * 2.0f * g_jitterY / static_cast<float>(height);
}

void DlssCamera(uintptr_t renderer, const uint8_t* camera, int level)
{
    uint32_t width, height;
    g_topJittered = false;
    // The main camera goes to the back end for DLSS and for GI; the jitter only with DLSS.
    const bool dlss = D3DLayerDlssScreen(&width, &height);
    if (!dlss && !D3DLayerScreen(&width, &height))
        return;
    static float frameFov = 1.0f; // the frame's camera as the 3D engine sets it (level 0)
    if (level == 0)
    {
        g_jitterSuspended = false; // the 3D engine sets the next frame's camera
        frameFov = *reinterpret_cast<const float*>(camera + kCamFov);
    }
    const float aspect = *reinterpret_cast<const float*>(camera + kCamAspect);
    const float fov = *reinterpret_cast<const float*>(camera + kCamFov);
    const float nearZ = *reinterpret_cast<const float*>(camera + kCamNear);
    if (g_jitterSuspended && dlss)
        g_suspendedCameras++;
    // Not the screen's camera: another aspect (shadow maps, the sky's band), or a narrow one (fov < 0.2) unless it is
    // the frame's own (the binoculars' second zoom, fov 0.13, had no main camera: no DLAA, the GI of the frame before;
    // letting every narrow camera through made objects flicker - some other camera of the screen's aspect took over).
    if (g_jitterSuspended || std::fabs(aspect - static_cast<float>(width) / static_cast<float>(height)) > 0.02f ||
        (fov < 0.2f && std::fabs(fov - frameFov) > 0.002f))
        return;
    float* proj = **reinterpret_cast<float***>(renderer + kProjStackOffset);
    if (nearZ >= 0.1f) // the main camera, not the weapon's
    {
        memcpy(g_dlssScene.view, **reinterpret_cast<float***>(renderer + kModelViewStackOffset), sizeof(g_dlssScene.view));
        memcpy(g_dlssScene.proj, proj, sizeof(g_dlssScene.proj));
        g_dlssScene.nearZ = nearZ;
        g_dlssScene.farZ = *reinterpret_cast<const float*>(camera + kCamFar);
        g_dlssScene.fov = fov;
        g_dlssScene.aspect = aspect;
        g_dlssScene.valid = 1;
        g_dlssScene.shiftU = dlss ? g_jitterX / static_cast<float>(width) : 0.0f;
        g_dlssScene.shiftV = dlss ? g_jitterY / static_cast<float>(height) : 0.0f;
        if (level <= 1) // not a reflection's camera
            D3DLayerCamera(&g_dlssScene);
    }
    if (!dlss)
        return;
    ApplyJitter(proj, width, height, 1.0f);
    g_topJittered = true;
    g_jitteredCameras[nearZ >= 0.1f ? 0 : 1]++;
}

// Replaces SetCamera's call of the view-projection update (thunk: rdx = the CCamera from rdi, r8 = SetCamera's caller).
extern "C" void CameraMatricesHook(uintptr_t renderer, const uint8_t* camera, uintptr_t caller)
{
    const int level = *reinterpret_cast<const int*>(kRecurseLevel);
    NoteCamera(caller, level, renderer == g_renderer, camera);
    if (renderer == g_renderer && GetCurrentThreadId() == g_rtTid)
        DlssCamera(renderer, camera, level);
    reinterpret_cast<void (*)(uintptr_t)>(kViewProjUpdateFn)(renderer);
}

// Before the first HDR post-processing batch list of the top level in a frame.
void DlssBeforeList(uintptr_t renderer)
{
    if (*reinterpret_cast<const int*>(kRecurseLevel) != 1 || *reinterpret_cast<const int*>(renderer + kBatchListOffset) != kListHdrPostProcess ||
        g_dlssSceneFrame == g_dlssFrame)
        return;
    g_dlssSceneFrame = g_dlssFrame;
    g_dlssScenes++;
    // CRYMT_SUNDUMP=1 (diagnostics): the level-1 lights at this point, once at frame 600: id, origin, flags and the first
    // 0x80 bytes as floats, with the camera, to logs\mt\crymt_sun.txt (finding the sun light's fields).
    static const bool sunDump = GetEnvironmentVariableW(L"CRYMT_SUNDUMP", nullptr, 0) != 0;
    if (sunDump && g_frame == 600)
    {
        const auto* r = reinterpret_cast<const uint8_t*>(renderer);
        const uint8_t* list = r + kRdLevelLights + 16;
        auto** lights = *reinterpret_cast<uint8_t** const*>(list);
        const int count = *reinterpret_cast<const int*>(list + 8);
        FILE* f = nullptr;
        if (fopen_s(&f, CryMTLog("crymt_sun.txt").c_str(), "w") == 0 && f)
        {
            const float* cam = reinterpret_cast<const float*>(r + kCameraOffset);
            fprintf(f, "camera bytes (floats):");
            for (int i = 0; i < 24; i++)
                fprintf(f, " %.3f", cam[i]);
            fprintf(f, "\nlevel-1 lights: %d\n", count);
            for (int i = 0; lights && i < count && i < 16; i++)
            {
                const uint8_t* l = lights[i];
                fprintf(f, "light %d id %d flags %08x origin %.1f %.1f %.1f\n  floats:", i, *reinterpret_cast<const int*>(l + kLightId),
                        *reinterpret_cast<const uint32_t*>(l + kLightFlags), *reinterpret_cast<const float*>(l + 4), *reinterpret_cast<const float*>(l + 8),
                        *reinterpret_cast<const float*>(l + 12));
                for (int k = 0; k < 0x80 / 4; k++)
                    fprintf(f, " %.3f", reinterpret_cast<const float*>(l)[k]);
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }
    // CRYMT_VISDUMP=1 (diagnostics): at frames 600 and 1500 the VisAreas and portals (CVisAreaManager) and every light
    // entity of the level (C3DEngine + 0x788) with its flags and whether the frame's list has it, to logs\mt\crymt_vis.txt.
    static const bool visDump = GetEnvironmentVariableW(L"CRYMT_VISDUMP", nullptr, 0) != 0;
    if (visDump && (g_frame == 600 || g_frame == 1500))
    {
        FILE* f = nullptr;
        if (fopen_s(&f, CryMTLog("crymt_vis.txt").c_str(), g_frame == 600 ? "w" : "a") == 0 && f)
        {
            const auto* mgr = *reinterpret_cast<const uint8_t* const*>(kVisAreaManagerGlobal);
            fprintf(f, "frame %llu, VisAreaManager %p\n", static_cast<unsigned long long>(g_frame), static_cast<const void*>(mgr));
            for (int list = 0; mgr && list < 2; list++)
            {
                const uint8_t* const* areas = *reinterpret_cast<const uint8_t* const* const*>(mgr + (list ? kVamPortals : kVamVisAreas));
                const int count = *reinterpret_cast<const int*>(mgr + (list ? kVamPortals : kVamVisAreas) + 8);
                fprintf(f, "%s: %d\n", list ? "portals" : "visareas", count);
                for (int i = 0; areas && i < count && i < 400; i++)
                {
                    const uint8_t* a = areas[i];
                    const float* box = reinterpret_cast<const float*>(a + kVisAreaBox);
                    const float* amb = reinterpret_cast<const float*>(a + kVisAreaAmbient);
                    const float* pts = *reinterpret_cast<const float* const*>(a + kVisAreaPoints);
                    const int np = *reinterpret_cast<const int*>(a + kVisAreaPoints + 8);
                    fprintf(f, "  %3d '%.32s' box %.1f %.1f %.1f - %.1f %.1f %.1f height %.2f amb %.3f %.3f %.3f b110 %d b118 %d b119 %d f11c %.2f b120 %d b121 %d b17c %d portal %d nosky %d points %d:",
                            i, reinterpret_cast<const char*>(a + kVisAreaName), box[0], box[1], box[2], box[3], box[4], box[5],
                            *reinterpret_cast<const float*>(a + kVisAreaHeight), amb[0], amb[1], amb[2], a[0x110], a[0x118], a[0x119],
                            *reinterpret_cast<const float*>(a + 0x11C), a[0x120], a[0x121], a[0x17C], a[0x17D], a[0x17E], np);
                    for (int k = 0; pts && k < np && k < 6; k++)
                        fprintf(f, " (%.1f %.1f %.1f)", pts[k * 3], pts[k * 3 + 1], pts[k * 3 + 2]);
                    fprintf(f, "\n");
                }
            }
            const auto* engine = *reinterpret_cast<const uint8_t* const*>(k3DEngineGlobal);
            const auto* const* ents = engine ? *reinterpret_cast<const uint8_t* const* const*>(engine + kEngineLightEntities) : nullptr;
            const int nents = engine ? *reinterpret_cast<const int*>(engine + kEngineLightEntities + 8) : 0;
            const auto* r = reinterpret_cast<const uint8_t*>(renderer);
            const uint8_t* list = r + kRdLevelLights + 16;
            auto** frameLights = *reinterpret_cast<uint8_t** const*>(list);
            const int frameCount = *reinterpret_cast<const int*>(list + 8);
            fprintf(f, "light entities: %d (sun entity %p), frame's lights %d\n", nents, engine ? *reinterpret_cast<void* const*>(engine + kEngineSunEntity) : nullptr, frameCount);
            for (int i = 0; ents && i < nents && i < 400; i++)
            {
                const uint8_t* e = ents[i];
                const uint8_t* l = e + kLightEntityLight;
                const float* o = reinterpret_cast<const float*>(l + kLightOrigin);
                const float* c = reinterpret_cast<const float*>(l + kLightColour);
                bool inFrame = false;
                for (int k = 0; frameLights && k < frameCount && k < 32; k++)
                {
                    const float* fo = reinterpret_cast<const float*>(frameLights[k] + kLightOrigin);
                    inFrame = inFrame || (fabsf(fo[0] - o[0]) < 0.01f && fabsf(fo[1] - o[1]) < 0.01f && fabsf(fo[2] - o[2]) < 0.01f);
                }
                fprintf(f, "  %3d %p rnd %08x light flags %08x origin %.2f %.2f %.2f radius %.2f colour %.2f %.2f %.2f%s\n", i, static_cast<const void*>(e),
                        *reinterpret_cast<const uint32_t*>(e + kRenderNodeFlags), *reinterpret_cast<const uint32_t*>(l + kLightFlags), o[0], o[1], o[2],
                        *reinterpret_cast<const float*>(l + kLightRadius), c[0], c[1], c[2], inFrame ? " IN FRAME" : "");
            }
            fclose(f);
        }
    }
    // CRYMT_LIGHTLOG=1 (diagnostics): the frame's level-1 lights whenever the set changes from the frame before (world
    // frames 100-1500), to logs\mt\crymt_lightlog.txt: lamps that come and go for a frame.
    static const bool lightLog = GetEnvironmentVariableW(L"CRYMT_LIGHTLOG", nullptr, 0) != 0;
    if (lightLog && g_frame >= 100 && g_frame <= 1500)
    {
        static std::string previous;
        const auto* r = reinterpret_cast<const uint8_t*>(renderer);
        const uint8_t* list = r + kRdLevelLights + 16;
        auto** lights = *reinterpret_cast<uint8_t** const*>(list);
        const int count = *reinterpret_cast<const int*>(list + 8);
        std::string now;
        char item[160];
        for (int i = 0; lights && i < count && i < 32; i++)
        {
            const float* o = reinterpret_cast<const float*>(lights[i] + kLightOrigin);
            const float* c = reinterpret_cast<const float*>(lights[i] + kLightColour);
            sprintf_s(item, " (%.1f %.1f %.1f r%.1f c%.2f f%x)", o[0], o[1], o[2], *reinterpret_cast<const float*>(lights[i] + kLightRadius),
                      c[0] + c[1] + c[2], *reinterpret_cast<const uint32_t*>(lights[i] + kLightFlags));
            now += item;
        }
        if (now != previous)
        {
            FILE* f = nullptr;
            if (fopen_s(&f, CryMTLog("crymt_lightlog.txt").c_str(), g_frame == 100 || previous.empty() ? "w" : "a") == 0 && f)
            {
                fprintf(f, "frame %llu: %d lights%s\n", static_cast<unsigned long long>(g_frame), count, now.c_str());
                fclose(f);
            }
            previous = now;
        }
    }
    // CRYMT_LIGHTDUMP=1 (diagnostics): every 1000 frames from 400 to 40000 the level-1 lights, all of CDLight as floats
    // and as hex, to logs\mt\crymt_lights.txt (finding the radius and type fields for the GI's voxel lamps).
    static const bool lightDump = GetEnvironmentVariableW(L"CRYMT_LIGHTDUMP", nullptr, 0) != 0;
    if (lightDump && g_frame >= 400 && g_frame <= 40000 && g_frame % 1000 == 400)
    {
        const auto* r = reinterpret_cast<const uint8_t*>(renderer);
        const uint8_t* list = r + kRdLevelLights + 16;
        auto** lights = *reinterpret_cast<uint8_t** const*>(list);
        const int count = *reinterpret_cast<const int*>(list + 8);
        FILE* f = nullptr;
        if (fopen_s(&f, CryMTLog("crymt_lights.txt").c_str(), g_frame == 400 ? "w" : "a") == 0 && f)
        {
            const float* cam = reinterpret_cast<const float*>(r + kCameraOffset);
            fprintf(f, "frame %llu camera %.2f %.2f %.2f, level-1 lights %d\n", static_cast<unsigned long long>(g_frame), cam[3], cam[7], cam[11], count);
            for (int i = 0; lights && i < count && i < 32; i++)
            {
                const uint8_t* l = lights[i];
                fprintf(f, "light %d id %d flags %08x origin %.2f %.2f %.2f\n", i, *reinterpret_cast<const int*>(l + kLightId),
                        *reinterpret_cast<const uint32_t*>(l + kLightFlags), *reinterpret_cast<const float*>(l + 4), *reinterpret_cast<const float*>(l + 8),
                        *reinterpret_cast<const float*>(l + 12));
                for (size_t k = 0; k < kLightBytes / 4; k++)
                {
                    const uint32_t u = reinterpret_cast<const uint32_t*>(l)[k];
                    float x;
                    memcpy(&x, &u, 4);
                    fprintf(f, "%s%03zx:%g/%08x", k % 8 ? " " : "\n  ", k * 4, x, u);
                }
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }
    if (g_jitterSwitchTick && GetTickCount() >= g_jitterSwitchTick)
    {
        g_jitterSwitchTick = 0;
        g_jitterSign = -g_jitterSign;
        g_jitterSignY = -g_jitterSignY;
        g_dlssReset = true;
    }
    if (!g_jitterModes.empty())
    {
        const DWORD now = ProcessMilliseconds();
        const size_t mode = now < g_jitterModeStart ? 0 : std::min<size_t>((now - g_jitterModeStart) / g_jitterModeStep, g_jitterModes.size() - 1);
        if (mode != g_jitterMode)
        {
            g_jitterMode = mode;
            g_jitterSign = g_jitterModes[mode].sx;
            g_jitterSignY = g_jitterModes[mode].sy;
            g_jitterReference = g_jitterModes[mode].reference;
            g_dlssReset = true;
        }
    }
    g_dlssScene.jitterX = g_jitterSign * g_jitterX;
    g_dlssScene.jitterY = g_jitterSignY * g_jitterY;
    if (g_jitterReference)
        g_dlssScene.valid = 0; // reference segment: jitter without DLSS
    g_dlssScene.frame = g_dlssFrame;
    g_dlssScene.reset = g_dlssReset;
    g_dlssReset = false;
    // The sun for the GI's voxel light: the level-1 light farthest away (its origin is the sun ~1000 km out; CRYMT_SUNDUMP
    // on island: one light, origin (-823130, 509032, 252289), colour at +0x1C 8.25 6.90 5.16 1).
    {
        const auto* r = reinterpret_cast<const uint8_t*>(renderer);
        const uint8_t* list = r + kRdLevelLights + 16;
        auto** lights = *reinterpret_cast<uint8_t** const*>(list);
        const int count = *reinterpret_cast<const int*>(list + 8);
        const float* cam = reinterpret_cast<const float*>(r + kCameraOffset);
        float best = 1e8f; // squared metres: 10 km
        memset(g_dlssScene.sun, 0, sizeof(g_dlssScene.sun));
        for (int i = 0; lights && i < count && i < 32; i++)
        {
            const float* o = reinterpret_cast<const float*>(lights[i] + kLightOrigin);
            const float d[3] = {o[0] - cam[3], o[1] - cam[7], o[2] - cam[11]};
            const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dd <= best)
                continue;
            best = dd;
            const float inv = 1.0f / sqrtf(dd);
            const float* c = reinterpret_cast<const float*>(lights[i] + kLightColour);
            const float sun[8] = {d[0] * inv, d[1] * inv, d[2] * inv, 1, c[0], c[1], c[2], 0};
            memcpy(g_dlssScene.sun, sun, sizeof(sun));
        }
        // The lamps: the level's lamps (SnapshotLamps) or, without them, the frame's point lights and projectors but
        // those within 1.5 m of the camera (the flashlight, muzzle flashes: they move with the view); the 8 nearest the
        // camera by their edge.
        struct Lamp
        {
            float key;
            float data[12];
        };
        static Lamp found[kLampSnapMax];
        int lampCount = 0;
        // The VisAreas: the 16 nearest the camera within 40 m of their box.
        struct Area
        {
            float key;
            int index;
        };
        static Area nearAreas[kVisSnapMax];
        int areaCount = 0;
        AcquireSRWLockShared(&g_lampLock);
        const bool level = g_lampSnapCount >= 0;
        for (int i = 0; level && i < g_lampSnapCount; i++)
            memcpy(found[lampCount++].data, g_lampSnap[i], sizeof(g_lampSnap[i]));
        for (int i = 0; i < g_visSnapCount; i++)
        {
            const float* a = g_visSnap[i];
            const float dx = std::max({a[0] - cam[3], cam[3] - a[2], 0.0f}), dy = std::max({a[1] - cam[7], cam[7] - a[3], 0.0f}),
                        dz = std::max({a[4] - cam[11], cam[11] - a[5], 0.0f});
            const float key = sqrtf(dx * dx + dy * dy + dz * dz);
            if (key < 40.0f)
                nearAreas[areaCount++] = {key, i};
        }
        std::sort(nearAreas, nearAreas + areaCount, [](const Area& a, const Area& b) { return a.key < b.key; });
        g_dlssScene.visAreaCount = static_cast<uint32_t>(std::min(areaCount, 16));
        for (uint32_t i = 0; i < g_dlssScene.visAreaCount; i++)
            memcpy(g_dlssScene.visAreas[i], g_visSnap[nearAreas[i].index], sizeof(g_dlssScene.visAreas[i]));
        ReleaseSRWLockShared(&g_lampLock);
        for (int i = 0; !level && lights && i < count && i < 32; i++)
        {
            const float* o = reinterpret_cast<const float*>(lights[i] + kLightOrigin);
            const float d[3] = {o[0] - cam[3], o[1] - cam[7], o[2] - cam[11]};
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= 1.5f * 1.5f && LampData(lights[i], found[lampCount].data))
                lampCount++;
        }
        for (int i = 0; i < lampCount; i++)
        {
            const float* l = found[i].data;
            const float d[3] = {l[0] - cam[3], l[1] - cam[7], l[2] - cam[11]};
            found[i].key = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - l[3];
        }
        std::sort(found, found + lampCount, [](const Lamp& a, const Lamp& b) { return a.key < b.key; });
        g_dlssScene.lampCount = static_cast<uint32_t>(std::min(lampCount, 8));
        for (uint32_t i = 0; i < g_dlssScene.lampCount; i++)
            memcpy(g_dlssScene.lamps[i], found[i].data, sizeof(found[i].data));
    }
    D3DLayerSceneDone(&g_dlssScene);
    g_dlssScene.valid = 0;
    // Post-processing and the HUD see the camera without jitter: undo it on the current projection.
    uint32_t width, height;
    if (g_topJittered && D3DLayerDlssScreen(&width, &height))
    {
        ApplyJitter(**reinterpret_cast<float***>(renderer + kProjStackOffset), width, height, -1.0f);
        reinterpret_cast<void (*)(uintptr_t)>(kViewProjUpdateFn)(renderer);
        g_topJittered = false;
    }
    g_jitterSuspended = true;
    g_dlssFrame++;
    NextJitter();
}

void DlssWriteStats(FILE* f)
{
    if (!g_dlssOn)
        return;
    fprintf(f, "dlss: scenes handed over %llu, jittered cameras: main %llu, weapon %llu, skipped while suspended %llu\n"
               "SetCamera calls (caller, level, real renderer, viewport, fov, aspect, near, far, count):\n",
            static_cast<unsigned long long>(g_dlssScenes), static_cast<unsigned long long>(g_jitteredCameras[0]),
            static_cast<unsigned long long>(g_jitteredCameras[1]), static_cast<unsigned long long>(g_suspendedCameras));
    AcquireSRWLockShared(&g_cameraSitesLock);
    for (const CameraSite& s : g_cameraSites)
        if (s.count)
            fprintf(f, "  %llx L%d %s %dx%d fov %.4f aspect %.4f near %.3f far %.1f  %llu\n", static_cast<unsigned long long>(s.caller), s.level,
                    s.real ? "real" : "fake", s.width, s.height, s.fov, s.aspect, s.nearZ, s.farZ, static_cast<unsigned long long>(s.count));
    ReleaseSRWLockShared(&g_cameraSitesLock);
}
} // namespace

// The renderer's matrices on top of its stacks (modelview, projection) when they changed since the last call; render
// thread only. Shadow maps (vf207, 0x380BC560) put the light's view-projection on the modelview stack and the identity
// as the projection: the D3D12 back end voxelizes their draws for GI (geometry around the player, not only in view).
bool CryMtPassMatrices(float* out)
{
    static float last[32];
    if (!g_renderer || GetCurrentThreadId() != g_rtTid)
        return false;
    const float* mv = **reinterpret_cast<float* const* const*>(g_renderer + kModelViewStackOffset);
    const float* proj = **reinterpret_cast<float* const* const*>(g_renderer + kProjStackOffset);
    if (!memcmp(last, mv, 64) && !memcmp(last + 16, proj, 64))
        return false;
    memcpy(last, mv, 64);
    memcpy(last + 16, proj, 64);
    memcpy(out, last, sizeof(last));
    return true;
}

// Pass names for frame debuggers (CRYMT_RENDERDOC or CRYMT_MARKERS): each outermost render list the render thread draws
// becomes a named range, "L<recursion level> <list> <filters>" (level 1 the scene, 2 shadow maps and reflections), so
// RenderDoc's event browser shows passes instead of thousands of loose draws. Nested calls (the own z-prepass hands
// its bases to the original one by one) stay inside their caller's range.
bool g_passNames;
int g_passNameDepth; // render thread only

void PassNameBegin(uintptr_t renderer, uint32_t filter, const char* tag)
{
    if (!g_passNames || GetCurrentThreadId() != g_rtTid || g_passNameDepth++ != 0)
        return;
    static const char* const kLists[] = {"?", "PREPROCESS", "GENERAL", "TERRAINLAYER", "DECAL", "WATER_VOLUMES", "TRANSP", "WATER",
                                         "HDRPOSTPROCESS", "AFTER_HDRPOSTPROCESS", "POSTPROCESS", "AFTER_POSTPROCESS", "SHADOW_GEN",
                                         "SHADOW_PASS", "REFRACTPASS"};
    static const char* const kFilters[] = {"GENERAL", "TRANSPARENT", "DETAIL", "Z", "GLOW", "SCATTER", "PREPROCESS", "MOTIONBLUR",
                                           "REFRACTIVE", "MULTILAYERS", "CAUSTICS", "CUSTOM_RENDER", "RAIN"};
    const uint32_t level = static_cast<uint32_t>(*reinterpret_cast<const int*>(kRecurseLevel)) & 0xF;
    const uint32_t list = static_cast<uint32_t>(*reinterpret_cast<const int*>(renderer + kBatchListOffset));
    const int afterWater = *reinterpret_cast<const int*>(renderer + 0xF0C);
    char text[192];
    int n = snprintf(text, sizeof(text), "L%u %s%s:", level, list < std::size(kLists) ? kLists[list] : "?", afterWater ? " after water" : "");
    for (int b = 0; b < static_cast<int>(std::size(kFilters)) && n < static_cast<int>(sizeof(text)) - 24; b++)
        if (filter & (1u << b))
            n += snprintf(text + n, sizeof(text) - n, " %s", kFilters[b]);
    if (tag)
        snprintf(text + n, sizeof(text) - n, " [%s]", tag);
    D3DLayerPassName(text);
}

void PassNameEnd()
{
    if (!g_passNames || GetCurrentThreadId() != g_rtTid || --g_passNameDepth != 0)
        return;
    D3DLayerPassName(nullptr);
}

extern "C" void __fastcall GpuBatchList(uintptr_t renderer, int first, int last, uint32_t filter)
{
    if (g_dlssOn && first != last && GetCurrentThreadId() == g_rtTid)
        DlssBeforeList(renderer);
    const bool named = g_passNames && first != last;
    if (named)
        PassNameBegin(renderer, filter, nullptr);
    if (!g_gpuCur || first == last || GetCurrentThreadId() != g_rtTid)
        g_batchList(renderer, first, last, filter);
    else
    {
        const uint32_t level = static_cast<uint32_t>(*reinterpret_cast<const int*>(kRecurseLevel)) & 0xF;
        const uint32_t list = static_cast<uint32_t>(*reinterpret_cast<const int*>(renderer + kBatchListOffset)) & 0xFF;
        GpuMark(kGpuList << 28 | level << 20 | list << 12 | (filter & 0xFFF));
        g_batchList(renderer, first, last, filter);
        GpuMark(kGpuAfterList << 28 | level << 20 | list << 12);
    }
    if (named)
        PassNameEnd();
}

// Jumps from the first 13 bytes of FUN_380A4480 to GpuBatchList; the trampoline runs them and continues.
bool InstallGpuProf()
{
    if (g_batchListHooked)
        return true;
    auto* fn = reinterpret_cast<uint8_t*>(kBatchListFn);
    if (memcmp(fn, kBatchListPrologue, sizeof(kBatchListPrologue)) != 0)
        return false;
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp)
        return false;
    const auto jumpAbs = [](uint8_t* at, uint64_t target) {
        at[0] = 0x49; at[1] = 0xBB; // movabs r11, target
        memcpy(at + 2, &target, 8);
        at[10] = 0x41; at[11] = 0xFF; at[12] = 0xE3; // jmp r11
    };
    memcpy(tramp, fn, sizeof(kBatchListPrologue));
    jumpAbs(tramp + sizeof(kBatchListPrologue), kBatchListFn + sizeof(kBatchListPrologue));
    DWORD old;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    g_batchList = reinterpret_cast<BatchListFn>(tramp);
    VirtualProtect(fn, sizeof(kBatchListPrologue), PAGE_EXECUTE_READWRITE, &old);
    jumpAbs(fn, reinterpret_cast<uint64_t>(&GpuBatchList));
    VirtualProtect(fn, sizeof(kBatchListPrologue), old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof(kBatchListPrologue));
    g_batchListHooked = true;
    return true;
}

namespace
{
bool RedirectCall(uintptr_t site, uintptr_t expectedTarget, const void* hook);
}

// The batch list detour (shared with the GPU pass timer) and SetCamera's view-projection call through a thunk:
//   mov rdx, rdi; mov r8, [rsp+0x210]; mov rax, CameraMatricesHook; jmp rax
void InstallDlssHooks()
{
    auto* thunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    bool camera = false;
    if (thunk)
    {
        const uint8_t code[] = {0x48, 0x89, 0xFA, 0x4C, 0x8B, 0x84, 0x24, 0x10, 0x02, 0x00, 0x00, 0x48, 0xB8};
        memcpy(thunk, code, sizeof(code));
        const auto hook = reinterpret_cast<uint64_t>(&CameraMatricesHook);
        memcpy(thunk + sizeof(code), &hook, 8);
        thunk[sizeof(code) + 8] = 0xFF;
        thunk[sizeof(code) + 9] = 0xE0;
        FlushInstructionCache(GetCurrentProcess(), thunk, 32);
        camera = RedirectCall(kSetCameraMatricesCall, kViewProjUpdateFn, thunk);
    }
    const bool lists = InstallGpuProf();
    wchar_t sign[32] = {};
    if (GetEnvironmentVariableW(L"CRYMT_DLSS_JITTER", sign, 32))
    {
        if ((sign[0] == L'+' || sign[0] == L'-') && (sign[1] == L'+' || sign[1] == L'-') && !sign[2]) // per axis: "+-" flips y
        {
            g_jitterSign = sign[0] == L'-' ? -1.0f : 1.0f;
            g_jitterSignY = sign[1] == L'-' ? -1.0f : 1.0f;
        }
        else if (sign[0] == L'-')
            g_jitterSign = g_jitterSignY = -1.0f;
        else if (wcscmp(sign, L"0") == 0)
            g_jitterOff = true;
        else if (wcsncmp(sign, L"switch:", 7) == 0) // flip the value given to DLSS after this many seconds (same run, same view)
            g_jitterSwitchTick = GetTickCount() + static_cast<DWORD>(_wtoi(sign + 7)) * 1000;
        else if (wcsncmp(sign, L"seq:", 4) == 0)
        {
            const wchar_t* p = sign + 4;
            g_jitterModeStart = static_cast<DWORD>(_wtoi(p)) * 1000;
            p = wcschr(p, L':');
            g_jitterModeStep = p ? static_cast<DWORD>(_wtoi(p + 1)) * 1000 : 3000;
            p = p ? wcschr(p + 1, L':') : nullptr;
            for (p = p ? p + 1 : nullptr; p && *p;)
            {
                if (*p == L'R')
                    g_jitterModes.push_back({1.0f, 1.0f, true}), p++;
                else if (p[1])
                    g_jitterModes.push_back({p[0] == L'-' ? -1.0f : 1.0f, p[1] == L'-' ? -1.0f : 1.0f, false}), p += 2;
                else
                    break;
                if (*p == L',')
                    p++;
            }
        }
    }
    NextJitter();
    char line[96];
    sprintf_s(line, "dlss hooks: camera=%d batch lists=%d jitter sign %+.0f", camera, lists, g_jitterSign);
    SetupLog(line);
    g_dlssOn = camera && lists;
}

void GpuLabelName(uint32_t key, char* out, size_t size)
{
    const uint32_t kind = key >> 28, level = key >> 20 & 0xF, list = key >> 12 & 0xFF, filter = key & 0xFFF;
    switch (kind)
    {
    case kGpuList: snprintf(out, size, "L%u.list%u.f%x", level, list, filter); break;
    case kGpuAfterList: snprintf(out, size, "L%u.after%u", level, list); break;
    case kGpuFlushStart: snprintf(out, size, "flushStart"); break;
    case kGpuAfterFlush: snprintf(out, size, "hud2d"); break;
    case kGpuEndFrame: snprintf(out, size, "endFrame"); break;
    case kGpuInner: snprintf(out, size, "%s", kGpuInnerSites[key & 0xFF].name); break;
    default: snprintf(out, size, "end"); break;
    }
}

void WriteGpuLog()
{
    if (!g_gpuProf && !g_gpuDev)
        return;
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_gpu.txt").c_str(), "w") != 0 || !f)
        return;
    const int labels = g_gpuLabelCount;
    fprintf(f, "seq");
    for (int pass = 0; pass < 2; pass++)
        for (int l = 0; l < labels; l++)
        {
            char name[48];
            GpuLabelName(g_gpuLabelKey[l], name, sizeof(name));
            fprintf(f, " %s%s", pass ? "cpu:" : "", name);
        }
    fprintf(f, "\n");
    for (uint32_t i = 0; i < kGpuFrames; i++)
    {
        if (!g_gpuSeq[i])
            continue;
        fprintf(f, "%u", g_gpuSeq[i]);
        for (int l = 0; l < labels; l++)
            fprintf(f, " %.3f", g_gpuMs[i][l]);
        for (int l = 0; l < labels; l++)
            fprintf(f, " %.3f", g_cpuMs[i][l]);
        fprintf(f, "\n");
    }
    fclose(f);
}

namespace
{
constexpr uintptr_t kHandled = 1;

void ExecuteCommand(Command& c)
{
    if (_mm_getcsr() != c.mxcsr)
        _mm_setcsr(c.mxcsr);
    const int64_t start = Now();
    const uint8_t* renderer = reinterpret_cast<const uint8_t*>(c.regs.rcx);
    if (c.slot == kEndEf3DSlot)
    {
        g_rtFrameSeq = c.count;
        memcpy(g_fake + kViewportOffset, renderer + kViewportOffset, 16);
        memcpy(g_fake + kCameraOffset, renderer + kCameraOffset, kCameraBytes);
        memcpy(g_snapMv, **reinterpret_cast<float* const* const*>(renderer + kModelViewStackOffset), 64);
        memcpy(g_snapProj, **reinterpret_cast<float* const* const*>(renderer + kProjStackOffset), 64);
        using QueryFn = uint64_t(__fastcall*)(const void*, uint64_t, uint64_t);
        for (int q = 0; q < 4; q++)
            g_querySnap[q] = reinterpret_cast<QueryFn>(g_orig[kEfQuery])(renderer, kSnapQueries[q], 0);
        g_snapSeq.store(c.count, std::memory_order_release);
    }
    if (c.kind == Kind::DrawDynVB)
    {
        using GetDynFn = void*(__fastcall*)(void*, int, int*, int);
        int offs = 0;
        void* dst = reinterpret_cast<GetDynFn>(g_orig[kGetDynVBPtr])(reinterpret_cast<void*>(c.regs.rcx), c.count, &offs, 0);
        if (dst)
            memcpy(dst, c.payload, c.count * kDynVertexSize);
    }
    if (c.kind == Kind::ProxyGet)
    {
        auto* p = static_cast<ProxyObj*>(const_cast<void*>(c.payload));
        p->real = reinterpret_cast<GetObjectRealFn>(g_realGetObject)(reinterpret_cast<void*>(c.regs.rcx), 1, 0xFFFFFFFFu, 0);
        g_rtBusyTicks.fetch_add(Now() - start, std::memory_order_relaxed);
        return;
    }
    if (c.kind == Kind::ProxyCall)
    {
        auto* s = static_cast<const ProxySnap*>(c.payload);
        ApplyProxyImage(s->proxy, s->obj, s->hasEntries, s->entries, s->hasObjData, s->objData);
        if (!s->proxy->real)
            return;
        uint64_t& objArg = s->objReg < 4 ? (&c.regs.rcx)[s->objReg] : c.stack[s->objReg - 4];
        objArg = reinterpret_cast<uint64_t>(s->proxy->real);
    }
    else if (c.kind == Kind::Call && g_proxies && c.slot < static_cast<uint32_t>(kAuxBase))
        TranslateQueued(&c.regs.rdx, 3, c.stack, ProxyStackArgs(c.slot));
    if (c.slot == kEndEf3DSlot)
    {
        GpuFrameBegin(c.regs.rcx, c.count);
        ProbeFlushStart();
        FlushLightsBegin();
        g_flushStartTick = Now();
        g_phaseFrames++;
    }
    else if (c.slot == kEndFrame)
        GpuMark(kGpuEndFrame << 28);
    ReplayCall(c.target, &c.regs, c.stack);
    if (c.slot == kAddParticles)
    {
        const auto* gs = reinterpret_cast<const ParticleGs*>(c.stack[2]);
        g_particleGsChecks++;
        if (gs->value != gs->expect)
            g_particleGsMismatch++;
    }
    if (g_occlusionReadMethod >= 0 && c.slot == static_cast<uint32_t>(kMethodReasonBase + g_occlusionReadMethod) && c.payload)
        static_cast<std::atomic<uint8_t>*>(const_cast<void*>(c.payload))->store(static_cast<uint8_t>(c.regs.rax), std::memory_order_relaxed);
    if (c.slot == kDownloadToVideoMemory && c.payload)
    {
        g_texUpdateChecks++;
        if (static_cast<int>(c.regs.rax) != *static_cast<const int*>(c.payload))
            g_texUpdateMismatch++;
    }
    if (c.slot == kPushFogContrib && c.payload)
    {
        g_fogChecks++;
        if (static_cast<uint16_t>(c.regs.rax) != *static_cast<const uint32_t*>(c.payload))
            g_fogMismatch++;
    }
    if (c.slot == kAddDLight && c.payload)
    {
        g_lightIdChecks++;
        if (*reinterpret_cast<const int*>(c.regs.rdx) != *static_cast<const int*>(c.payload))
            g_lightIdMismatch++;
    }
    if ((c.slot == kSetCamera || c.slot == kSetViewport) && c.payload)
    {
        g_mirrorChecks++;
        if (!MirrorMatches(renderer, *static_cast<const MirrorExpect*>(c.payload), c.slot == kSetViewport))
            g_mirrorMismatch++;
    }
    if (c.slot == kEndEf3DSlot)
    {
        GpuMark(kGpuAfterFlush << 28);
        if (g_frustumTest)
            ProbeCompare();
        CarryBack(true);
    }
    else if (c.slot == kEndFrame)
        GpuFrameEnd();
    const int64_t end = Now();
    g_rtBusyTicks.fetch_add(end - start, std::memory_order_relaxed);
    if (c.slot == kBeginFrame || c.slot == kEndFrame)
    {
        // The snapshot is kept across frame boundaries; check that they leave the view state alone.
        g_boundaryChecks++;
        const uint64_t before = g_boundaryMismatch;
        const bool vp = memcmp(g_fake + kViewportOffset, renderer + kViewportOffset, 16) != 0;
        const bool cam = memcmp(g_fake + kCameraOffset, renderer + kCameraOffset, 0x80) != 0;
        const bool mv = memcmp(g_snapMv, **reinterpret_cast<float* const* const*>(renderer + kModelViewStackOffset), 64) != 0;
        const bool pr = memcmp(g_snapProj, **reinterpret_cast<float* const* const*>(renderer + kProjStackOffset), 64) != 0;
        if (c.slot == kBeginFrame)
        {
            g_bfChanged[0] += vp;
            g_bfChanged[1] += cam;
            g_bfChanged[2] += mv;
            g_bfChanged[3] += pr;
        }
        if (vp || cam || mv || pr)
            g_boundaryMismatch++;
        if (c.slot == kEndFrame && g_boundaryMismatch != before)
            g_endFrameMismatch++;
    }
    if (c.slot == kEndEf3DSlot)
    {
        g_rtEndEfTicks.fetch_add(end - start, std::memory_order_relaxed);
        g_rtFlush[c.count % kMaxDetail] = static_cast<float>((end - start) * g_qpcToMs);
        const bool signalled = g_preDoneSeq.load(std::memory_order_relaxed) == c.count;
        g_rtPre[c.count % kMaxDetail] = static_cast<float>(((signalled ? g_preDoneTime : end) - start) * g_qpcToMs);
        static uint64_t lastPhase[4];
        const uint64_t phase[4] = {g_phaseTicks[0], g_phaseTicks[1], g_phaseTicks[2], g_shadowDrawTicks};
        for (int k = 0; k < 4; k++)
        {
            g_rtPhase[k][c.count % kMaxDetail] = static_cast<float>((phase[k] - lastPhase[k]) * g_qpcToMs);
            lastPhase[k] = phase[k];
        }
        // The snapshot stands in for the state main would see after the flush; count frames where
        // the flush left a different view state behind.
        g_snapChecks++;
        if (memcmp(g_fake + kViewportOffset, renderer + kViewportOffset, 16) != 0 ||
            memcmp(g_snapMv, **reinterpret_cast<float* const* const*>(renderer + kModelViewStackOffset), 64) != 0 ||
            memcmp(g_snapProj, **reinterpret_cast<float* const* const*>(renderer + kProjStackOffset), 64) != 0)
            g_snapMismatch++;
        using QueryFn = uint64_t(__fastcall*)(const void*, uint64_t, uint64_t);
        for (int q = 0; q < 4; q++)
            if (reinterpret_cast<QueryFn>(g_orig[kEfQuery])(renderer, kSnapQueries[q], 0) != g_querySnap[q])
                g_queryMismatch++;
    }
}

DWORD WINAPI RenderThreadMain(void*)
{
    for (;;)
    {
        const uint64_t r = g_read.load(std::memory_order_relaxed);
        if (r == g_write.load(std::memory_order_acquire))
        {
            bool got = false;
            for (int i = 0; i < 20000 && !got; i++)
            {
                _mm_pause();
                got = r != g_write.load(std::memory_order_acquire);
            }
            if (!got)
            {
                g_rtSleeping.store(1, std::memory_order_seq_cst);
                if (r == g_write.load(std::memory_order_seq_cst))
                    WaitForSingleObject(g_rtWake, 2);
                g_rtSleeping.store(0, std::memory_order_relaxed);
            }
            continue;
        }
        ExecuteCommand(g_ring[r & (kRingSize - 1)]);
        g_read.store(r + 1, std::memory_order_release);
    }
}

void RefreshShadows()
{
    g_shadowFog = *reinterpret_cast<uint8_t*>(g_renderer + kFogFlagOffset);
    g_shadowFrameId = *reinterpret_cast<int*>(g_renderer + kFrameIdOffset);
    g_shadowFrameIdAll = *reinterpret_cast<int*>(g_renderer + kFrameIdAllOffset);
    g_shadowLevel = *reinterpret_cast<int*>(kRecurseLevel);
    g_auxObject = *reinterpret_cast<uintptr_t*>(g_renderer + kAuxObjectOffset);
    if (g_auxObject)
        g_shadowAuxFlags = *reinterpret_cast<uint32_t*>(g_auxObject + kAuxFlagsOffset);
}

bool g_noPump;

void PumpSentMessages()
{
    if (g_noPump)
        return;
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}

void WaitIdle(int reason)
{
    if (!g_async)
        return;
    NoteSyncUnderLock(reason);
    const uint64_t target = g_write.load(std::memory_order_relaxed);
    const int64_t start = Now();
    const bool lockDropped = ReleaseEngineLockForWait();
    for (uint32_t spin = 0; g_read.load(std::memory_order_acquire) != target; spin++)
    {
        _mm_pause();
        if ((spin & 0xFFF) == 0xFFF)
            PumpSentMessages();
    }
    if (lockDropped)
        RetakeEngineLock();
    g_pendingMeshCount = 0; // the render thread has run every queued mesh edit
    const int64_t waited = Now() - start;
    NoteFrameWait(reason, waited);
    g_lastSyncSlot = reason;
    g_snapInvalid = true;
    g_invalidReason = 1000 + reason;
    g_async = false;
    RunDeferredFrees();
    if (!g_recordingNow) // recorded commands keep their arena data until the flush replays them
        g_arenaUsed = 0;
    g_dynPtr = nullptr;
    g_mainWaitTicks += waited;
    if (reason >= 0)
    {
        SyncStats& s = g_sync[reason];
        s.waits++;
        s.waitTicks += waited;
        if (!g_firstSyncSeen)
        {
            g_firstSyncSeen = true;
            s.first++;
            s.firstTicks += start - g_dispatchTime;
        }
    }
}

} // namespace

void WaitIdlePublic(int reason)
{
    WaitIdle(reason);
}

namespace
{
Command& BeginCommand(uint32_t slot)
{
    if (g_recordingNow)
    {
        Command& c = g_recCommands.emplace_back();
        c.kind = Kind::Call;
        c.slot = slot;
        c.mxcsr = _mm_getcsr();
        c.count = 0;
        c.target = g_orig[slot];
        c.payload = nullptr;
        return c;
    }
    if (!g_async)
    {
        RefreshShadows();
        g_async = true;
    }
    const uint64_t w = g_write.load(std::memory_order_relaxed);
    if (w - g_read.load(std::memory_order_acquire) >= kRingSize)
    {
        g_ringFullWaits++;
        while (w - g_read.load(std::memory_order_acquire) >= kRingSize)
            _mm_pause();
    }
    Command& c = g_ring[w & (kRingSize - 1)];
    c.kind = Kind::Call;
    c.slot = slot;
    c.mxcsr = _mm_getcsr();
    c.count = 0;
    c.target = g_orig[slot];
    c.payload = nullptr;
    return c;
}

void PublishCommand()
{
    if (g_recordingNow)
        return;
    g_write.store(g_write.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    g_commands++;
    if (g_rtSleeping.load(std::memory_order_seq_cst))
        SetEvent(g_rtWake);
}

void StartRenderThread()
{
    if (g_rtThread)
        return;
    g_rtWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_fake = static_cast<uint8_t*>(VirtualAlloc(nullptr, kFakeSize + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_fakeMvStack = g_fake + kFakeSize;
    g_fakeProjStack = g_fake + kFakeSize + 64;
    *reinterpret_cast<float**>(g_fakeMvStack) = g_snapMv;
    *reinterpret_cast<float**>(g_fakeProjStack) = g_snapProj;
    *reinterpret_cast<uint8_t**>(g_fake + kModelViewStackOffset) = g_fakeMvStack;
    *reinterpret_cast<uint8_t**>(g_fake + kProjStackOffset) = g_fakeProjStack;
    {
        const auto aux = *reinterpret_cast<uintptr_t*>(g_renderer + 0x2D898);
        const auto vt = aux ? *reinterpret_cast<uintptr_t*>(aux) : 0;
        char line[1200];
        int n = sprintf_s(line, "aux object %llx vtable %llx slots:", static_cast<unsigned long long>(aux), static_cast<unsigned long long>(vt));
        for (int i = 0; vt && i < 40; i++)
        {
            const auto fn = reinterpret_cast<const uintptr_t*>(vt)[i];
            if (fn - kRenderBase >= kRenderSize)
                break;
            n += sprintf_s(line + n, sizeof(line) - n, " %d:%llX", i, static_cast<unsigned long long>(fn - kRenderBase));
        }
        SetupLog(line);
    }
    g_rtThread = CreateThread(nullptr, 16 * 1024 * 1024, RenderThreadMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_rtTid);
    SetThreadPriority(g_rtThread, THREAD_PRIORITY_ABOVE_NORMAL);
}

void* ArenaAlloc(size_t bytes)
{
    bytes = (bytes + 15) & ~size_t{15};
    if (g_arenaUsed + bytes > kArenaSize)
        return nullptr;
    void* p = g_arena + g_arenaUsed;
    g_arenaUsed += bytes;
    return p;
}
} // namespace

void* ArenaAllocPublic(size_t bytes)
{
    return ArenaAlloc(bytes);
}

namespace
{

void TrackViewState(uint32_t slot, const CallFrame* f)
{
    switch (slot)
    {
    case kSet2DMode:
        g_matrixDepth += (f->rdx & 0xFF) ? 1 : -1;
        break;
    case kFontSetState:
        g_matrixDepth++;
        break;
    case kFontRestoreState:
        g_matrixDepth--;
        break;
    case kSetViewport:
    case kResetToDefault:
        g_snapInvalid = true;
        g_invalidReason = static_cast<int>(slot);
        break;
    case kBeginFrame: // measured: changes the modelview matrix every frame, never viewport, camera or projection
        g_snapMatrixInvalid = true;
        g_invalidReason = static_cast<int>(slot);
        break;
    }
}

bool SnapshotUsable()
{
    return !g_snapInvalid && !g_snapMatrixInvalid && g_matrixDepth == 0 && g_snapSeq.load(std::memory_order_acquire) == g_frameSeq;
}

void DeferCall(uint32_t slot, CallFrame* f, const uintptr_t* rbp)
{
    TrackViewState(slot, f);
    Command& c = BeginCommand(slot);
    c.regs = *f;
    memcpy(c.stack, rbp + 6, sizeof(c.stack));
    if (slot == kSetClearColor)
    {
        memcpy(c.stack, reinterpret_cast<const void*>(f->rdx), 12);
        c.regs.rdx = reinterpret_cast<uint64_t>(c.stack);
    }
    PublishCommand();
}

} // namespace

int QueueOcclusionRead(int method, const uint64_t* frame)
{
    const uintptr_t re = frame[3];
    QueryReady* slot = nullptr;
    for (int i = 0; i < g_queryReadyCount && !slot; i++)
        if (g_queryReady[i].re == re)
            slot = &g_queryReady[i];
    if (!slot)
    {
        if (g_queryReadyCount == 64)
            return 0;
        slot = &g_queryReady[g_queryReadyCount++];
        slot->re = re;
        slot->ready.store(0, std::memory_order_relaxed);
    }
    Command& c = BeginCommand(0);
    c.slot = kMethodReasonBase + method;
    c.target = reinterpret_cast<void*>(g_methodOrig[method]);
    c.regs = {};
    c.regs.rcx = re;
    memset(c.stack, 0, sizeof(c.stack));
    c.payload = &slot->ready;
    PublishCommand();
    g_occlusionReadsQueued++;
    return 1 + slot->ready.load(std::memory_order_relaxed);
}

// CRenderMesh::SetIndices (mesh, inds, count; the indices copied) or SetChunk (mesh, material, four ints, chunk id,
// flag; all by value) from the fill thunk, queued; the mesh counts as pending until WaitIdle.
bool QueueMeshEdit(int method, const uint64_t* frame)
{
    const uintptr_t mesh = frame[3];
    if (!MeshEditPending(mesh) && g_pendingMeshCount == kPendingMeshMax)
        return false;
    const bool indices = IsMeshMethod(method, kMeshSetIndices);
    const uint32_t count = static_cast<uint32_t>(frame[1]);
    void* inds = nullptr;
    float* box = nullptr;
    if (IsMeshMethod(method, kMeshSetBBox))
    {
        box = static_cast<float*>(ArenaAlloc(24));
        if (!box)
            return false;
        memcpy(box, reinterpret_cast<const void*>(frame[2]), 12);
        memcpy(box + 3, reinterpret_cast<const void*>(frame[1]), 12);
    }
    if (indices && frame[2] && count)
    {
        inds = count <= 0x100000 ? ArenaAlloc(count * 2) : nullptr;
        if (!inds)
        {
            g_arenaFullWaits++;
            return false;
        }
        memcpy(inds, reinterpret_cast<const void*>(frame[2]), count * 2);
    }
    Command& c = BeginCommand(0);
    c.slot = kMethodReasonBase + method;
    c.target = reinterpret_cast<void*>(g_methodOrig[method]);
    c.regs = {};
    c.regs.rcx = mesh;
    c.regs.rdx = indices ? reinterpret_cast<uint64_t>(inds) : box ? reinterpret_cast<uint64_t>(box) : frame[2];
    c.regs.r8 = box ? reinterpret_cast<uint64_t>(box + 3) : frame[1];
    c.regs.r9 = frame[0];
    memset(c.stack, 0, sizeof(c.stack));
    if (!indices)
        memcpy(c.stack, frame + 9, 4 * sizeof(uint64_t));
    PublishCommand();
    if (!MeshEditPending(mesh))
        g_pendingMeshes[g_pendingMeshCount++] = mesh;
    (indices ? g_meshIndicesQueued : g_meshChunksQueued)++;
    return true;
}

void QueueResourceAddRef(int method, const uint64_t* frame)
{
    Command& c = BeginCommand(0);
    c.slot = kMethodReasonBase + method;
    c.target = reinterpret_cast<void*>(g_methodOrig[method]);
    c.regs = {};
    c.regs.rcx = frame[3];
    memset(c.stack, 0, sizeof(c.stack));
    PublishCommand();
    g_resourceAddRefsQueued++;
}

// Special calls that change renderer state and are queued even when the render thread is idle.
bool IsSpecialMutator(uint32_t idx)
{
    if (idx >= static_cast<uint32_t>(kAuxBase))
    {
        const int slot = static_cast<int>(idx) - kAuxBase;
        return slot == kAuxFlush || kAuxSpecs[slot].copy;
    }
    return idx == kBeginFrame || idx == kSwapFogFlag || idx == kDrawDynVB || idx == kGetDynVBPtr || idx == kDraw2dLabel;
}

namespace
{
uint64_t& ArgRef(Command& c, int arg)
{
    switch (arg)
    {
    case 1:
        return c.regs.rdx;
    case 2:
        return c.regs.r8;
    case 3:
        return c.regs.r9;
    default:
        return c.stack[arg - 4];
    }
}

// Queues an aux geometry call with its pointer arguments copied into the arena. Returns false when
// the arena is full; the caller then syncs and calls the object directly.
bool DeferAux(int slot, CallFrame* f, const uintptr_t* rbp)
{
    Command& c = BeginCommand(kAuxBase + slot);
    c.regs = *f;
    memcpy(c.stack, rbp + 6, sizeof(c.stack));
    for (const ArgCopy& a : kAuxSpecs[slot].args)
    {
        if (a.elem == 0)
            break;
        uint64_t& value = ArgRef(c, a.arg);
        if (!value)
            continue;
        const size_t count = a.countArg < 0 ? 1 : static_cast<uint32_t>(ArgRef(c, a.countArg));
        const size_t bytes = count * a.elem;
        if (bytes == 0)
            continue;
        void* p = ArenaAlloc(bytes);
        if (!p)
        {
            g_arenaFullWaits++;
            return false;
        }
        memcpy(p, reinterpret_cast<const void*>(value), bytes);
        value = reinterpret_cast<uint64_t>(p);
    }
    PublishCommand();
    g_auxDeferred++;
    return true;
}

uintptr_t HandleAux(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    const int slot = static_cast<int>(idx) - kAuxBase;
    if (slot == kAuxGetFlags)
    {
        *reinterpret_cast<uint32_t*>(f->rdx) = g_shadowAuxFlags;
        f->rax = f->rdx;
        return kHandled;
    }
    if (slot == kAuxNoOp)
    {
        f->rax = 0;
        return kHandled;
    }
    if (slot == kAuxFlush || kAuxSpecs[slot].copy)
    {
        const uint32_t newFlags = slot == kAuxSetFlags ? *reinterpret_cast<const uint32_t*>(f->rdx) : 0;
        const bool queued = slot == kAuxFlush ? (DeferCall(idx, f, rbp), true) : DeferAux(slot, f, rbp);
        if (queued)
        {
            if (slot == kAuxSetFlags)
                g_shadowAuxFlags = newFlags;
            f->rax = 0;
            return kHandled;
        }
    }
    WaitIdle(static_cast<int>(idx));
    return reinterpret_cast<uintptr_t>(g_orig[idx]);
}

// EF_ADDDlight in the fill window: main writes the id the renderer will assign (light count of the level, -1 for
// fake lights per EF_IsFakeDLight, -1 beyond 32 lights) and queues the call; the render thread checks the id.
int g_fillLightCount[9];
void* g_fillLights[9][32]; // the pointers EF_ADDDlight stores in the level's list, for EF_Query 0x1B

uintptr_t DeferAddDLight(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    auto* light = reinterpret_cast<int*>(f->rdx);
    const int level = g_shadowLevel;
    auto* expect = static_cast<int*>(ArenaAlloc(sizeof(int)));
    if (!light || !expect || level < 1 || level > 8)
    {
        WaitIdle(static_cast<int>(idx));
        return reinterpret_cast<uintptr_t>(g_orig[idx]);
    }
    using IsFakeFn = bool(__fastcall*)(void*, void*);
    int id;
    if (reinterpret_cast<IsFakeFn>(g_orig[kIsFakeDLight])(reinterpret_cast<void*>(f->rcx), light))
        id = -1;
    else if (g_fillLightCount[level] > 0x1F)
        id = -1;
    else
    {
        id = g_fillLightCount[level]++;
        g_fillLights[level][id] = light;
    }
    *light = id;
    *expect = id;
    Command& c = BeginCommand(idx);
    c.regs = *f;
    memcpy(c.stack, rbp + 6, sizeof(c.stack));
    c.payload = expect;
    PublishCommand();
    f->rax = 0;
    return kHandled;
}

// EF_GetObject(temp) in the fill window: a proxy initialized like a reused temp object (EF_GetObject 0x38102010).
uintptr_t ProxyGetObject(CallFrame* f)
{
    if (!g_proxies)
        g_proxies = static_cast<ProxyObj*>(VirtualAlloc(nullptr, 2 * kProxyMax * sizeof(ProxyObj), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_proxies || g_proxyUsed == kProxyMax)
    {
        WaitIdle(kEfGetObject);
        return reinterpret_cast<uintptr_t>(g_orig[kEfGetObject]);
    }
    ProxyObj* p = g_proxies + static_cast<size_t>(g_proxyHalf) * kProxyMax + g_proxyUsed++;
    memset(p, 0, sizeof(*p));
    const uint32_t one = *reinterpret_cast<const uint32_t*>(kConstOne);
    auto set16 = [&](size_t off, uint16_t v) { memcpy(p->obj + off, &v, 2); };
    auto set32 = [&](size_t off, uint32_t v) { memcpy(p->obj + off, &v, 4); };
    const uint64_t vt = kRenderObjectVtable;
    memcpy(p->obj, &vt, 8);
    set16(0xC, 0xFFFF);
    set16(0xE, 0xFFFF);
    set16(0xCE, 0xFFFF);
    set16(0xD0, 0xFFFF);
    for (size_t off : {0x44, 0x48, 0x4C, 0x50, 0xE4, 0xF8})
        set32(off, one);
    g_frameObjects.push_back(reinterpret_cast<uintptr_t>(p));
    g_proxyCount++;
    f->rax = reinterpret_cast<uint64_t>(p);
    return kHandled;
}

ProxySnap* SnapProxy(ProxyObj* p, int objReg)
{
    auto* s = static_cast<ProxySnap*>(ArenaAlloc(sizeof(ProxySnap)));
    if (!s)
        return nullptr;
    s->proxy = p;
    s->objReg = objReg;
    s->hasEntries = p->hasEntries;
    s->hasObjData = p->hasObjData;
    memcpy(s->obj, p->obj, sizeof(s->obj));
    memcpy(s->entries, p->entries, sizeof(s->entries));
    memcpy(s->objData, p->objData, sizeof(s->objData));
    if (p->hasObjData && *reinterpret_cast<const uint64_t*>(p->objData + 0x18) != kEmptyArray)
        g_proxyDataArrays++;
    return s;
}

// EF_AddEf (slot 114: pRE, SShaderItem*, object, list, after-water) with a proxy object.
uintptr_t RecordProxyAddEf(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    auto* item = static_cast<uint8_t*>(ArenaAlloc(0x18));
    ProxySnap* s = SnapProxy(reinterpret_cast<ProxyObj*>(f->r9), 3);
    if (!item || !s)
    {
        WaitIdle(static_cast<int>(idx));
        ApplyProxyLive(reinterpret_cast<ProxyObj*>(f->r9));
        f->r9 = reinterpret_cast<uint64_t>(reinterpret_cast<ProxyObj*>(f->r9)->real);
        return reinterpret_cast<uintptr_t>(g_orig[idx]);
    }
    memcpy(item, reinterpret_cast<const void*>(f->r8), 0x18);
    Command& c = BeginCommand(idx);
    c.kind = Kind::ProxyCall;
    c.regs = *f;
    c.regs.r8 = reinterpret_cast<uint64_t>(item);
    memcpy(c.stack, rbp + 6, sizeof(c.stack));
    c.payload = s;
    PublishCommand();
    f->rax = 0;
    return kHandled;
}

// EF_AddPolygonToScene (slot 92: SShaderItem&, numPts, verts, tangs, object, inds, ninds, nAW, bMerge) copies the
// polygon into renderer arrays and returns the object, or an equal object merged earlier. Its callers in the fill
// window (decals, 3D+0x9C49F/0xE67CA) ignore the result, so the call is queued with its data copied.

bool DeferAddPolygon(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    const uint64_t* stack = reinterpret_cast<const uint64_t*>(rbp + 6);
    const uint32_t pts = static_cast<uint32_t>(f->r8);
    const uint64_t obj = stack[1];
    const uint32_t ninds = stack[2] ? static_cast<uint32_t>(stack[3]) : 0;
    if (!f->rdx || !f->r9 || pts == 0 || pts > 4096 || ninds > 3 * 4096 || (InProxyPool(obj) && !IsProxy(obj)))
        return false;
    auto* item = static_cast<uint8_t*>(ArenaAlloc(0x18));
    auto* verts = static_cast<uint8_t*>(ArenaAlloc(pts * kDynVertexSize));
    auto* tangs = stack[0] ? static_cast<uint8_t*>(ArenaAlloc(pts * kTangentsBytes)) : nullptr;
    auto* inds = ninds ? static_cast<uint8_t*>(ArenaAlloc(ninds * 2)) : nullptr;
    ProxySnap* s = IsProxy(obj) ? SnapProxy(reinterpret_cast<ProxyObj*>(obj), 5) : nullptr;
    if (!item || !verts || (stack[0] && !tangs) || (ninds && !inds) || (IsProxy(obj) && !s))
    {
        g_arenaFullWaits++;
        return false;
    }
    memcpy(item, reinterpret_cast<const void*>(f->rdx), 0x18);
    memcpy(verts, reinterpret_cast<const void*>(f->r9), pts * kDynVertexSize);
    if (tangs)
        memcpy(tangs, reinterpret_cast<const void*>(stack[0]), pts * kTangentsBytes);
    if (inds)
        memcpy(inds, reinterpret_cast<const void*>(stack[2]), ninds * 2);
    Command& c = BeginCommand(idx);
    c.kind = s ? Kind::ProxyCall : Kind::Call;
    c.regs = *f;
    c.regs.rdx = reinterpret_cast<uint64_t>(item);
    c.regs.r9 = reinterpret_cast<uint64_t>(verts);
    memcpy(c.stack, stack, sizeof(c.stack));
    c.stack[0] = reinterpret_cast<uint64_t>(tangs);
    if (inds)
        c.stack[2] = reinterpret_cast<uint64_t>(inds);
    c.payload = s;
    PublishCommand();
    g_polysQueued++;
    f->rax = obj;
    return true;
}

// EF_AddParticlesToScene (slot 87, 0x38104840: SShaderItem&, object, IParticleVertexCreator*, SParticleRenderInfo&,
// nAW, bool& canUseGS) merges the object, takes a CREParticle that keeps the creator and the first 24 bytes of the
// render info, and leaves canUseGS = in && r_UseGSParticles && technique flag 0x100000 (FUN_3816ED90, +0x18). Its
// caller (CParticleContainer::Render, 3D+0x71E9F) keeps only canUseGS; containers outlive the flush (deferred release).
bool DeferAddParticles(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    const uint64_t* stack = reinterpret_cast<const uint64_t*>(rbp + 6);
    auto* canUseGs = reinterpret_cast<uint8_t*>(stack[2]);
    const uint64_t obj = f->r8;
    if (!f->rdx || !stack[0] || !canUseGs || (InProxyPool(obj) && !IsProxy(obj)))
        return false;
    auto* item = static_cast<uint64_t*>(ArenaAlloc(0x18));
    auto* info = static_cast<uint8_t*>(ArenaAlloc(0x18));
    auto* gs = static_cast<ParticleGs*>(ArenaAlloc(sizeof(ParticleGs)));
    ProxySnap* s = IsProxy(obj) ? SnapProxy(reinterpret_cast<ProxyObj*>(obj), 2) : nullptr;
    if (!item || !info || !gs || (IsProxy(obj) && !s))
    {
        g_arenaFullWaits++;
        return false;
    }
    memcpy(item, reinterpret_cast<const void*>(f->rdx), 0x18);
    memcpy(info, reinterpret_cast<const void*>(stack[0]), 0x18);
    const auto* shader = reinterpret_cast<const uint8_t*>(item[0]);
    const int tech = std::max(static_cast<int>(item[2]), 0);
    const uint8_t* technique = shader && tech < *reinterpret_cast<const int*>(shader + kShaderTechniqueCount)
                                   ? reinterpret_cast<const uint8_t* const*>(*reinterpret_cast<const uint64_t*>(shader + kShaderTechniques))[tech]
                                   : nullptr;
    gs->value = *canUseGs;
    gs->expect = *canUseGs && *reinterpret_cast<const int*>(kCvarUseGSParticles) && technique &&
                 (*reinterpret_cast<const uint32_t*>(technique + kTechniqueFlags) & kTechniqueGSParticles);
    Command& c = BeginCommand(idx);
    c.kind = s ? Kind::ProxyCall : Kind::Call;
    c.regs = *f;
    c.regs.rdx = reinterpret_cast<uint64_t>(item);
    memcpy(c.stack, stack, sizeof(c.stack));
    c.stack[0] = reinterpret_cast<uint64_t>(info);
    c.stack[2] = reinterpret_cast<uint64_t>(&gs->value);
    c.payload = s;
    PublishCommand();
    g_particlesQueued++;
    *canUseGs = gs->expect;
    f->rax = obj;
    return true;
}

// DownLoadToVideoMemory with an existing texture id (CTextureCache::GetTexture, 3D+0xB6AA0: terrain sector textures,
// many while explosions deform the terrain) only updates level 0 from the data and returns the id: queued with the
// data copied, the render thread checks the id.
bool DeferTextureUpdate(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    const uint64_t* stack = reinterpret_cast<const uint64_t*>(rbp + 6);
    const int id = static_cast<int>(stack[5]);
    const int w = static_cast<int>(f->r8);
    const int h = static_cast<int>(f->r9);
    if (id < 1 || !f->rdx || stack[6] || w < 1 || h < 1 || w > 4096 || h > 4096)
        return false;
    const int bytes = reinterpret_cast<int(__fastcall*)(int, int, int, int, int)>(kTextureDataSizeFn)(w, h, 1, 1, static_cast<int>(stack[0]));
    if (bytes <= 0 || bytes > 16 * 1024 * 1024)
        return false;
    void* data = ArenaAlloc(static_cast<size_t>(bytes));
    auto* expect = static_cast<int*>(ArenaAlloc(sizeof(int)));
    if (!data || !expect)
    {
        g_arenaFullWaits++;
        return false;
    }
    memcpy(data, reinterpret_cast<const void*>(f->rdx), static_cast<size_t>(bytes));
    *expect = id;
    Command& c = BeginCommand(idx);
    c.regs = *f;
    c.regs.rdx = reinterpret_cast<uint64_t>(data);
    memcpy(c.stack, stack, sizeof(c.stack));
    c.payload = expect;
    PublishCommand();
    g_texUpdatesQueued++;
    f->rax = static_cast<uint32_t>(id);
    return true;
}

// Main, render thread idle (after a sync point): proxies among the arguments become their real objects.
void TranslateProxies(uint64_t* regs, int nregs, uint64_t* stack, int nstack)
{
    for (int i = 0; i < nregs + nstack; i++)
    {
        uint64_t& v = i < nregs ? regs[i] : stack[i - nregs];
        if (!InProxyPool(v))
            continue;
        ProxyObj* p = ProxyContaining(v);
        const uint64_t off = v - reinterpret_cast<uint64_t>(p);
        ApplyProxyLive(p);
        uint8_t* real = p->real;
        if (!real)
            continue;
        if (off < kRenderObjectBytes)
            v = reinterpret_cast<uint64_t>(real + off);
        else if (off < offsetof(ProxyObj, entries) + kObjEntryBytes)
        {
            uint8_t* e = RealEntries(real);
            if (!e)
                continue;
            v = reinterpret_cast<uint64_t>(e + (off - offsetof(ProxyObj, entries)));
        }
        else if (off < offsetof(ProxyObj, objData) + kObjDataBytes)
        {
            uint8_t* od = reinterpret_cast<ObjSlot1Fn>(g_objSlot1)(real, 1);
            if (!od)
                continue;
            v = reinterpret_cast<uint64_t>(od + (off - offsetof(ProxyObj, objData)));
        }
        else
            continue;
        g_proxyTranslated++;
    }
}

bool HasProxyArg(const uint64_t* regs, int nregs, const uint64_t* stack, int nstack)
{
    for (int i = 0; i < nregs; i++)
        if (InProxyPool(regs[i]))
            return true;
    for (int i = 0; i < nstack; i++)
        if (InProxyPool(stack[i]))
            return true;
    return false;
}

// CRenderObject slots 0/1 and CRenderMesh::Render, patched over the fill thunks (InstallProxyHooks).
using MeshRenderFn = void(__fastcall*)(void*, void*, void*, void*, char);
uintptr_t g_meshRender;
constexpr int kObjSlotReason = 600; // 600/601 slot 0/1 on a real object, 602 CRenderMesh::Render

extern "C" void __fastcall ObjSlot0Hook(uint8_t* obj, uint8_t** out)
{
    if (IsProxy(reinterpret_cast<uint64_t>(obj)))
    {
        auto* p = reinterpret_cast<ProxyObj*>(obj);
        if (!p->hasEntries)
        {
            const uint32_t one = 1;
            memcpy(p->entries, &one, 4);
            memcpy(p->entries + 0x14, &one, 4);
            p->hasEntries = true;
        }
        if (out)
        {
            out[0] = p->entries;
            out[1] = p->entries + 0x14;
        }
        return;
    }
    if (g_fillWindow && GetCurrentThreadId() == g_mainThread)
        WaitIdle(kObjSlotReason);
    reinterpret_cast<ObjSlot0Fn>(g_objSlot0)(obj, out);
}

extern "C" uint8_t* __fastcall ObjSlot1Hook(uint8_t* obj, char create)
{
    if (IsProxy(reinterpret_cast<uint64_t>(obj)))
    {
        auto* p = reinterpret_cast<ProxyObj*>(obj);
        if (!p->hasObjData)
        {
            if (!create)
                return nullptr;
            const uint64_t empty = kEmptyArray;
            memcpy(p->objData + 0x18, &empty, 8);
            p->hasObjData = true;
        }
        return p->objData;
    }
    if (g_fillWindow && GetCurrentThreadId() == g_mainThread)
        WaitIdle(kObjSlotReason + 1);
    return reinterpret_cast<ObjSlot1Fn>(g_objSlot1)(obj, create);
}

extern "C" void __fastcall MeshRenderHook(void* mesh, uint8_t* params, uint8_t* obj, void* mtl, char flag)
{
    const bool onMain = GetCurrentThreadId() == g_mainThread;
    if (onMain && !g_mainMeshes.empty())
        HandOffMesh(reinterpret_cast<uintptr_t>(mesh));
    if (g_fillWindow && onMain && IsProxy(reinterpret_cast<uint64_t>(obj)))
    {
        auto* paramsCopy = static_cast<uint8_t*>(ArenaAlloc(0xB0));
        ProxySnap* s = SnapProxy(reinterpret_cast<ProxyObj*>(obj), 2);
        if (paramsCopy && s)
        {
            memcpy(paramsCopy, params, 0xB0);
            Command& c = BeginCommand(0);
            c.kind = Kind::ProxyCall;
            c.slot = kObjSlotReason + 2;
            c.target = reinterpret_cast<void*>(g_meshRender);
            c.regs = {};
            c.regs.rcx = reinterpret_cast<uint64_t>(mesh);
            c.regs.rdx = reinterpret_cast<uint64_t>(paramsCopy);
            c.regs.r8 = reinterpret_cast<uint64_t>(obj);
            c.regs.r9 = reinterpret_cast<uint64_t>(mtl);
            memset(c.stack, 0, sizeof(c.stack));
            c.stack[0] = static_cast<uint8_t>(flag);
            c.payload = s;
            PublishCommand();
            return;
        }
    }
    if (g_fillWindow && onMain)
    {
        WaitIdle(kObjSlotReason + 2);
        if (IsProxy(reinterpret_cast<uint64_t>(obj)))
        {
            ApplyProxyLive(reinterpret_cast<ProxyObj*>(obj));
            obj = reinterpret_cast<ProxyObj*>(obj)->real;
        }
    }
    reinterpret_cast<MeshRenderFn>(g_meshRender)(mesh, params, obj, mtl, flag);
}

// CRenderMesh slot 24 (0x38158900): adds the mesh's render elements for an object
// (mesh, object, list, after water, material, sort), called from 3D+0xC1508/0xC1920 about 85 times a frame.
using MeshAddReFn = void(__fastcall*)(void*, uint8_t*, uint64_t, uint64_t, void*, uint64_t);
uintptr_t g_meshAddRe;

extern "C" void __fastcall MeshAddReHook(void* mesh, uint8_t* obj, uint64_t list, uint64_t afterWater, void* mtl, uint64_t sort)
{
    const bool onMain = GetCurrentThreadId() == g_mainThread;
    if (onMain && !g_mainMeshes.empty())
        HandOffMesh(reinterpret_cast<uintptr_t>(mesh));
    if (g_fillWindow && onMain && IsProxy(reinterpret_cast<uint64_t>(obj)))
        if (ProxySnap* s = SnapProxy(reinterpret_cast<ProxyObj*>(obj), 1))
        {
            Command& c = BeginCommand(0);
            c.kind = Kind::ProxyCall;
            c.slot = kObjSlotReason + 3;
            c.target = reinterpret_cast<void*>(g_meshAddRe);
            c.regs = {};
            c.regs.rcx = reinterpret_cast<uint64_t>(mesh);
            c.regs.rdx = reinterpret_cast<uint64_t>(obj);
            c.regs.r8 = list;
            c.regs.r9 = afterWater;
            memset(c.stack, 0, sizeof(c.stack));
            c.stack[0] = reinterpret_cast<uint64_t>(mtl);
            c.stack[1] = sort;
            c.payload = s;
            PublishCommand();
            return;
        }
    if (g_fillWindow && onMain)
    {
        WaitIdle(kObjSlotReason + 3);
        if (IsProxy(reinterpret_cast<uint64_t>(obj)))
        {
            ApplyProxyLive(reinterpret_cast<ProxyObj*>(obj));
            obj = reinterpret_cast<ProxyObj*>(obj)->real;
        }
    }
    reinterpret_cast<MeshAddReFn>(g_meshAddRe)(mesh, obj, list, afterWater, mtl, sort);
}

// CRenderMesh slot 40 (0x381587A0): adds the chunks' render elements for (object, material, list, after water),
// via FUN_38101D80; called from 3D+0xB2C6A about 42 times a frame.
using MeshAddRe2Fn = void(__fastcall*)(void*, uint8_t*, void*, uint64_t, uint64_t);
uintptr_t g_meshAddRe2;

extern "C" void __fastcall MeshAddRe2Hook(void* mesh, uint8_t* obj, void* mtl, uint64_t list, uint64_t afterWater)
{
    const bool onMain = GetCurrentThreadId() == g_mainThread;
    if (onMain && !g_mainMeshes.empty())
        HandOffMesh(reinterpret_cast<uintptr_t>(mesh));
    if (g_fillWindow && onMain && IsProxy(reinterpret_cast<uint64_t>(obj)))
        if (ProxySnap* s = SnapProxy(reinterpret_cast<ProxyObj*>(obj), 1))
        {
            Command& c = BeginCommand(0);
            c.kind = Kind::ProxyCall;
            c.slot = kObjSlotReason + 4;
            c.target = reinterpret_cast<void*>(g_meshAddRe2);
            c.regs = {};
            c.regs.rcx = reinterpret_cast<uint64_t>(mesh);
            c.regs.rdx = reinterpret_cast<uint64_t>(obj);
            c.regs.r8 = reinterpret_cast<uint64_t>(mtl);
            c.regs.r9 = list;
            memset(c.stack, 0, sizeof(c.stack));
            c.stack[0] = afterWater;
            c.payload = s;
            PublishCommand();
            return;
        }
    if (g_fillWindow && onMain)
    {
        WaitIdle(kObjSlotReason + 4);
        if (IsProxy(reinterpret_cast<uint64_t>(obj)))
        {
            ApplyProxyLive(reinterpret_cast<ProxyObj*>(obj));
            obj = reinterpret_cast<ProxyObj*>(obj)->real;
        }
    }
    reinterpret_cast<MeshAddRe2Fn>(g_meshAddRe2)(mesh, obj, mtl, list, afterWater);
}

void InstallProxyHooks()
{
    int objFirst = -1;
    int meshFirst = -1;
    for (const RendererClass& cls : g_rendererClasses)
    {
        if (cls.vtable == kRenderObjectVtable)
            objFirst = cls.first;
        if (strcmp(cls.name, ".?AUCRenderMesh@@") == 0)
            meshFirst = cls.first;
    }
    if (objFirst < 0 || meshFirst < 0 || g_methodOrig[objFirst] != 0x38100760 || g_methodOrig[objFirst + 1] != 0x38108C00 ||
        g_methodOrig[meshFirst + 47] != 0x38161B40 || g_methodOrig[meshFirst + 24] != 0x38158900 ||
        g_methodOrig[meshFirst + 40] != 0x381587A0)
    {
        SetupLog("proxy hooks: renderer vtables differ, not installed");
        return;
    }
    g_objSlot0 = g_methodOrig[objFirst];
    g_objSlot1 = g_methodOrig[objFirst + 1];
    g_meshRender = g_methodOrig[meshFirst + 47];
    PatchRendererMethod(objFirst, reinterpret_cast<const void*>(&ObjSlot0Hook));
    PatchRendererMethod(objFirst + 1, reinterpret_cast<const void*>(&ObjSlot1Hook));
    PatchRendererMethod(meshFirst + 47, reinterpret_cast<const void*>(&MeshRenderHook));
    g_meshAddRe = g_methodOrig[meshFirst + 24];
    PatchRendererMethod(meshFirst + 24, reinterpret_cast<const void*>(&MeshAddReHook));
    g_meshAddRe2 = g_methodOrig[meshFirst + 40];
    PatchRendererMethod(meshFirst + 40, reinterpret_cast<const void*>(&MeshAddRe2Hook));
    g_translateProxies = &TranslateProxies;
    SetupLog("proxy hooks installed");
}

// Fill mode view setters (SetCamera, SetViewport): run on the fake for main, then queued for the render thread
// with a copy of the camera and what the fake ended up with, which the render thread compares after the real
// call. The fake stays main's exact view, so the view getters never need a sync point.
bool IsMirrored(uint32_t idx)
{
    return g_fakeOwned && (idx == kSetCamera || idx == kSetViewport);
}

uintptr_t MirrorCall(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    const size_t argBytes = idx == kSetCamera ? kCameraArgBytes : 0;
    auto* arg = argBytes ? static_cast<uint8_t*>(ArenaAlloc(argBytes)) : nullptr;
    auto* expect = static_cast<MirrorExpect*>(ArenaAlloc(sizeof(MirrorExpect)));
    if ((argBytes && !arg) || !expect)
    {
        WaitIdle(static_cast<int>(idx));
        return reinterpret_cast<uintptr_t>(g_orig[idx]);
    }
    if (arg)
        memcpy(arg, reinterpret_cast<const void*>(f->rdx), argBytes);
    CallFrame onFake = *f;
    onFake.rcx = reinterpret_cast<uint64_t>(g_fake);
    g_depth++;
    g_mirroring = true;
    ReplayCall(g_orig[idx], &onFake, rbp + 6);
    g_mirroring = false;
    g_depth--;
    MirrorCapture(g_fake, *expect);
    Command& c = BeginCommand(idx);
    c.regs = *f;
    if (arg)
        c.regs.rdx = reinterpret_cast<uint64_t>(arg);
    memcpy(c.stack, rbp + 6, sizeof(c.stack));
    c.payload = expect;
    PublishCommand();
    // The camera loads both matrices, so the fake is exact again after BeginFrame changed the view matrix.
    if (idx == kSetCamera)
        g_snapMatrixInvalid = false;
    g_mirrorCalls++;
    f->rax = 0;
    return kHandled;
}

uintptr_t HandleSpecial(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    if (idx >= static_cast<uint32_t>(kAuxBase))
        return HandleAux(idx, f, rbp);
    switch (idx)
    {
    case kGetViewport:
    case kGetCamera:
    case kProjectToScreen:
    case kUnProjectFromScreen: // the 3D engine, ~0.2 a frame on harbor; its nested matrix and viewport calls land here too
    case kGetModelView:
    case kGetProjection:
        // Set2DMode and the font state push matrices only, so the viewport and camera stay valid at
        // any matrix depth.
        // In fill mode the fake gets every SetCamera/SetViewport (MirrorCall), so it stays exact after sync points.
        if (!(idx == kGetViewport || idx == kGetCamera
                  ? g_fakeOwned || (!g_snapInvalid && g_snapSeq.load(std::memory_order_acquire) == g_frameSeq)
                  : SnapshotUsable()))
        {
            if (g_async)
            {
                const int r = g_snapInvalid || g_snapMatrixInvalid ? g_invalidReason : g_matrixDepth != 0 ? 2000 + g_matrixDepth : 3000;
                for (int k = 0; k < 16; k++)
                    if (g_missReason[k][0] == r || g_missReason[k][1] == 0)
                    {
                        g_missReason[k][0] = r;
                        g_missReason[k][1]++;
                        break;
                    }
            }
            break;
        }
        g_snapHits++;
        f->rcx = reinterpret_cast<uint64_t>(g_fake);
        return reinterpret_cast<uintptr_t>(g_orig[idx]);
    case kGetFrameID:
        f->rax = static_cast<uint32_t>((f->rdx & 0xFF) ? g_shadowFrameIdAll : g_shadowFrameId);
        return kHandled;
    case kDraw2dLabel:
    {
        // Draw2dLabel(x, y, size, float* color, bool center, fmt, ...) formats into 256 bytes and
        // appends to the text list; format here, queue the call with "%s".
        static const char kPercentS[] = "%s";
        char* text = static_cast<char*>(ArenaAlloc(256));
        float* color = rbp[6] ? static_cast<float*>(ArenaAlloc(16)) : nullptr;
        if (!text || (rbp[6] && !color))
            break;
        vsnprintf(text, 256, reinterpret_cast<const char*>(rbp[8]), reinterpret_cast<va_list>(const_cast<uintptr_t*>(rbp + 9)));
        if (color)
            memcpy(color, reinterpret_cast<const void*>(rbp[6]), 16);
        Command& c = BeginCommand(idx);
        c.regs = *f;
        memcpy(c.stack, rbp + 6, sizeof(c.stack));
        c.stack[0] = reinterpret_cast<uint64_t>(color);
        c.stack[2] = reinterpret_cast<uint64_t>(kPercentS);
        c.stack[3] = reinterpret_cast<uint64_t>(text);
        PublishCommand();
        f->rax = 0;
        return kHandled;
    }
    case kSwapFogFlag:
    {
        const uint8_t old = g_shadowFog;
        DeferCall(idx, f, rbp);
        g_shadowFog = static_cast<uint8_t>(f->rdx);
        f->rax = old;
        return kHandled;
    }
    case kEfQuery:
    {
        const uint32_t id = static_cast<uint32_t>(f->rdx);
        if (id == kQueryRecurseLevel)
        {
            f->rax = static_cast<uint64_t>(static_cast<int64_t>(g_shadowLevel));
            return kHandled;
        }
        if (g_snapSeq.load(std::memory_order_acquire) == g_frameSeq)
            for (int q = 0; q < 4; q++)
                if (id == kSnapQueries[q])
                {
                    g_queryHits++;
                    f->rax = g_querySnap[q];
                    return kHandled;
                }
        break;
    }
    case kBeginFrame:
        DeferCall(idx, f, rbp);
        g_shadowFrameId++;
        g_shadowFrameIdAll++;
        f->rax = 0;
        return kHandled;
    case kGetDynVBPtr:
    {
        if (static_cast<int>(f->r9) != 0)
            break;
        const uint32_t verts = static_cast<uint32_t>(f->rdx);
        void* p = ArenaAlloc(verts * kDynVertexSize);
        if (!p)
        {
            g_arenaFullWaits++;
            break;
        }
        *reinterpret_cast<int*>(f->r8) = 0;
        g_dynPtr = p;
        g_dynVerts = verts;
        f->rax = reinterpret_cast<uint64_t>(p);
        return kHandled;
    }
    case kDrawDynVB:
    {
        if (!g_dynPtr)
            break;
        Command& c = BeginCommand(idx);
        c.kind = Kind::DrawDynVB;
        c.regs = *f;
        memcpy(c.stack, rbp + 6, sizeof(c.stack));
        c.payload = g_dynPtr;
        c.count = std::min<uint32_t>(g_dynVerts, static_cast<uint32_t>(f->r9));
        g_dynPtr = nullptr;
        PublishCommand();
        f->rax = 0;
        return kHandled;
    }
    }
    WaitIdle(static_cast<int>(idx));
    return reinterpret_cast<uintptr_t>(g_orig[idx]);
}
} // namespace

// rbp points at the gate's saved rbp: [1] is the caller's return address, [6..] its stack arguments.
extern "C" uintptr_t GateC(uint32_t idx, CallFrame* f, const uintptr_t* rbp)
{
    const uintptr_t orig = reinterpret_cast<uintptr_t>(g_orig[idx]);
    // CREMesh for a chunk of a mesh main edits directly (SetChunk -> slot 27 -> EF_CreateRE(9), renderer code calling
    // the renderer): from the pool, since a constructor here would race the render thread on the element list.
    if (idx == kCreateRE && g_terrainOwn && static_cast<int>(f->rdx) == kRETypeMesh && g_fillWindow && g_async &&
        GetCurrentThreadId() == g_mainThread)
    {
        if (!g_rePool.empty())
        {
            f->rax = reinterpret_cast<uint64_t>(g_rePool.back());
            g_rePool.pop_back();
            g_rePoolUsed++;
            return kHandled;
        }
        g_rePoolEmpty++;
        WaitIdle(static_cast<int>(idx));
        return orig;
    }
    if (GetCurrentThreadId() != g_mainThread || g_depth != 0)
        return orig;
    const uintptr_t ret = rbp[1];
    if (ret - kRenderBase < kRenderSize)
        return orig;

    if (g_mode != 2)
    {
        if (g_inWindow)
        {
            if (g_frame == g_traceFrame && g_traceCount < kMaxTrace)
                g_trace[g_traceCount++] = {idx, ret, f->rdx, Now() - g_windowStart};
            if (idx == kEfQuery)
                g_queryHist[f->rdx & 0xFF]++;
            SlotStats& s = g_slots[idx];
            s.calls++;
            if (s.stamp != g_frame)
            {
                s.stamp = g_frame;
                s.framesSeen++;
                s.firstSum += Now() - g_windowStart;
            }
        }
        return orig;
    }

    if (IsMirrored(idx))
        return MirrorCall(idx, f, rbp);
    if (g_fillWindow)
        switch (idx)
        {
        case kStartEf:
            // The flush draws copies of the lights, so the frame start is queued; main tracks the level and
            // its own light counter for the ids.
            DeferCall(idx, f, rbp);
            if (g_shadowLevel == 0)
                g_fogContribs = 0;
            if (g_shadowLevel >= 0 && g_shadowLevel < 8)
                g_fillLightCount[g_shadowLevel + 1] = 0;
            g_shadowLevel++;
            f->rax = 0;
            return kHandled;
        case kResetLevelField:
            if (g_shadowLevel >= 0 && g_shadowLevel <= 8)
                g_fillLightCount[g_shadowLevel] = 0; // it zeroes the level's light count
            [[fallthrough]];
        case kUpdateDLight:
            DeferCall(idx, f, rbp);
            f->rax = 0;
            return kHandled;
        case kIsFakeDLight:
            return orig;
        case kAddDLight:
            return DeferAddDLight(idx, f, rbp);
        case kEfGetObject:
            if ((f->rdx & 0xFF) && static_cast<int32_t>(f->r8) < 0)
                return ProxyGetObject(f);
            // A permanent object by id (render nodes such as clouds, 3D+0x97018): only adds it to the frame's list.
            if (!(f->rdx & 0xFF) && static_cast<uint32_t>(f->r8) < kPermObjectMax)
                if (void* obj = g_permObjects[static_cast<uint32_t>(f->r8)].load(std::memory_order_relaxed))
                {
                    DeferCall(idx, f, rbp);
                    f->rax = reinterpret_cast<uint64_t>(obj);
                    g_frameObjects.push_back(f->rax);
                    g_permQueued++;
                    return kHandled;
                }
            break;
        case kEfQuery:
            // 0x1B: the n-th light of the recursion level (renderer+0xF40 + level*16), 0 past the end.
            if (static_cast<uint32_t>(f->rdx) == kQueryLevelLight && g_shadowLevel >= 1 && g_shadowLevel <= 8)
            {
                const uint32_t n = static_cast<uint32_t>(f->r8);
                f->rax = n < static_cast<uint32_t>(g_fillLightCount[g_shadowLevel]) ? reinterpret_cast<uint64_t>(g_fillLights[g_shadowLevel][n]) : 0;
                g_levelLightQueries++;
                return kHandled;
            }
            break;
        case kCreateRenderMesh:
            if (g_terrainOwn && g_async && g_meshListHooked)
            {
                // Only system copies (no external vertex container p13, no p15); precache p16 off, so the render thread
                // creates the buffers when it first draws the mesh. The new mesh belongs to main for this fill window.
                auto* stack = const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(rbp + 6));
                if (stack[8] == 0 && (stack[10] & 0xFF) == 0)
                {
                    stack[11] &= ~uint64_t{0xFF};
                    g_inMeshCreate = true;
                    g_depth++;
                    ReplayCall(g_orig[idx], f, rbp + 6);
                    g_depth--;
                    g_inMeshCreate = false;
                    if (f->rax)
                    {
                        g_mainMeshes.push_back(f->rax);
                        g_mainMeshesCreated++;
                    }
                    return kHandled;
                }
            }
            break;
        case kEfAddEf:
            // A chunk element of a mesh main created: the render thread will use the mesh from now on.
            if (!g_mainMeshes.empty() && f->rdx && *reinterpret_cast<const uintptr_t*>(f->rdx) == kREMeshVtable)
                HandOffMesh(*reinterpret_cast<const uintptr_t*>(f->rdx + 0x48));
            if (IsProxy(f->r9))
                return RecordProxyAddEf(idx, f, rbp);
            if (f->r8 && !InProxyPool(f->r9))
            {
                // A permanent or real object: only the shader item needs a copy.
                auto* item = static_cast<uint8_t*>(ArenaAlloc(0x18));
                if (!item)
                    break;
                memcpy(item, reinterpret_cast<const void*>(f->r8), 0x18);
                Command& c = BeginCommand(idx);
                c.regs = *f;
                c.regs.r8 = reinterpret_cast<uint64_t>(item);
                memcpy(c.stack, rbp + 6, sizeof(c.stack));
                PublishCommand();
                f->rax = 0;
                return kHandled;
            }
            break;
        case kAddPolygon:
            if (DeferAddPolygon(idx, f, rbp))
                return kHandled;
            break;
        case kAddParticles:
            if (DeferAddParticles(idx, f, rbp))
                return kHandled;
            break;
        case kSetFog:
            if (rbp[6])
                if (auto* color = static_cast<float*>(ArenaAlloc(16)))
                {
                    memcpy(color, reinterpret_cast<const void*>(rbp[6]), 12);
                    Command& c = BeginCommand(idx);
                    c.regs = *f;
                    memcpy(c.stack, rbp + 6, sizeof(c.stack));
                    c.stack[0] = reinterpret_cast<uint64_t>(color);
                    PublishCommand();
                    f->rax = 0;
                    return kHandled;
                }
            break;
        case kSetTerrainAONodes:
            if (f->rdx)
            {
                // PodArray: elements, count, allocated. The engine refills its array every frame.
                const auto* src = reinterpret_cast<const uint8_t*>(f->rdx);
                const int count = *reinterpret_cast<const int*>(src + 8);
                auto* arr = static_cast<uint8_t*>(ArenaAlloc(16));
                auto* elems = count > 0 ? static_cast<uint8_t*>(ArenaAlloc(static_cast<size_t>(count) * kSectorTextureSetBytes)) : nullptr;
                if (arr && count >= 0 && (count == 0 || elems))
                {
                    if (count)
                        memcpy(elems, *reinterpret_cast<const void* const*>(src), static_cast<size_t>(count) * kSectorTextureSetBytes);
                    *reinterpret_cast<uint8_t**>(arr) = elems;
                    reinterpret_cast<int*>(arr)[2] = count;
                    reinterpret_cast<int*>(arr)[3] = count;
                    Command& c = BeginCommand(idx);
                    c.regs = *f;
                    c.regs.rdx = reinterpret_cast<uint64_t>(arr);
                    memcpy(c.stack, rbp + 6, sizeof(c.stack));
                    PublishCommand();
                    f->rax = 0;
                    return kHandled;
                }
            }
            break;
        case kPushFogContrib:
            if (auto* color = static_cast<uint8_t*>(ArenaAlloc(16)))
                if (auto* expect = static_cast<uint32_t*>(ArenaAlloc(sizeof(uint32_t))))
                {
                    memcpy(color, reinterpret_cast<const void*>(f->rdx), 16);
                    *expect = g_fogContribs < 0xFFFF ? g_fogContribs++ : 0xFFFF;
                    Command& c = BeginCommand(idx);
                    c.regs = *f;
                    c.regs.rdx = reinterpret_cast<uint64_t>(color);
                    memcpy(c.stack, rbp + 6, sizeof(c.stack));
                    c.payload = expect;
                    PublishCommand();
                    g_fogQueued++;
                    f->rax = *expect;
                    return kHandled;
                }
            break;
        }
    if (idx == kDownloadToVideoMemory && g_fillOn && DeferTextureUpdate(idx, f, rbp))
        return kHandled;
    // Queued calls get their proxies replaced on the render thread (ExecuteCommand); snapshot queries ignore
    // their argument. Any other renderer call that gets a proxy runs after a sync point on the real object.
    const bool snapQuery = idx == kEfQuery && (IsSnapQuery(static_cast<uint32_t>(f->rdx)) || (f->rdx & 0xFF) == kQueryRecurseLevel);
    // Direct slots are getters without object arguments: their registers past the real arguments hold whatever the
    // caller left there.
    if (g_fillWindow && g_proxyUsed && g_policy[idx] != kDefer && g_policy[idx] != kDirect && !snapQuery &&
        idx < static_cast<uint32_t>(kAuxBase) &&
        HasProxyArg(&f->rdx, 3, rbp + 6, ProxyStackArgs(idx)))
    {
        if (idx == kEfQuery)
            g_proxyQueryHist[f->rdx & 0xFF]++;
        // Any other renderer call that gets a proxy runs after the render thread caught up, on the real object.
        if (g_async)
            NoteFillSync(-1 - static_cast<int>(idx), ret, f->rdx, f->r8);
        WaitIdle(static_cast<int>(idx));
        TranslateProxies(&f->rdx, 3, const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(rbp + 6)), ProxyStackArgs(idx));
    }
    const uint8_t policy = g_policy[idx];
    if (policy == kDirect)
        return orig;
    if (policy == kSync || (!g_async && (g_frame < g_warmupFrames || (policy == kSpecial && !IsSpecialMutator(idx)))))
    {
        // Run the call here and track the depth, so engine code it calls back into executes inline
        // instead of being queued behind it.
        if (g_async) // also outside the fill window: sync sites of the game update
            NoteFillSync(-1 - static_cast<int>(idx), ret, f->rdx, f->r8);
        WaitIdle(static_cast<int>(idx));
        g_depth++;
        ReplayCall(g_orig[idx], f, rbp + 6);
        g_depth--;
        if (idx == kPushFogContrib && static_cast<uint16_t>(f->rax) != 0xFFFF)
            g_fogContribs = static_cast<uint16_t>(f->rax) + 1u;
        return kHandled;
    }
    if (!g_async)
        RefreshShadows();
    if (policy == kDefer)
    {
        DeferCall(idx, f, rbp);
        f->rax = 0;
        return kHandled;
    }
    return HandleSpecial(idx, f, rbp);
}

namespace
{
using EndEfFn = void*(__fastcall*)(void*, void*, void*, void*);

struct RenderWorker
{
    HANDLE thread = nullptr;
    HANDLE wake = nullptr;
    HANDLE done = nullptr;
    void* args[4] = {};
    void* result = nullptr;
    unsigned int mxcsr = 0;
    unsigned int x87 = 0;
};
RenderWorker g_worker;

DWORD WINAPI RenderWorkerMain(void*)
{
    for (;;)
    {
        WaitForSingleObject(g_worker.wake, INFINITE);
        _mm_setcsr(g_worker.mxcsr);
        _controlfp_s(nullptr, g_worker.x87, _MCW_PC | _MCW_RC | _MCW_EM);
        g_topStart = Now();
        g_frameLastNestedEnd = g_topStart;
        g_worker.result = reinterpret_cast<EndEfFn>(g_orig[kEndEf3DSlot])(g_worker.args[0], g_worker.args[1], g_worker.args[2], g_worker.args[3]);
        g_topTicks += Now() - g_topStart;
        g_lastNestedEndTicks += g_frameLastNestedEnd - g_topStart;
        g_topCount++;
        SetEvent(g_worker.done);
    }
}

void* CallEndEfOnWorker(void* self, void* a, void* b, void* c)
{
    if (!g_worker.thread)
    {
        g_worker.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_worker.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_worker.thread = CreateThread(nullptr, 16 * 1024 * 1024, RenderWorkerMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }
    g_worker.args[0] = self;
    g_worker.args[1] = a;
    g_worker.args[2] = b;
    g_worker.args[3] = c;
    g_worker.mxcsr = _mm_getcsr();
    _controlfp_s(&g_worker.x87, 0, 0);
    SetEvent(g_worker.wake);
    WaitForSingleObject(g_worker.done, INFINITE);
    return g_worker.result;
}

uint64_t RecKey(uintptr_t caster, uintptr_t frustum)
{
    return static_cast<uint64_t>(caster) * 0x9E3779B97F4A7C15ull ^ frustum;
}

// At dispatch, render thread idle: record every shadow caster's Render for every frustum the flush will prepare
// (CRYMT_SHADOWREC; see CasterRecording). Main stands in the fill window at the shadow pass's recursion level.
void RecordShadowCasters(uint8_t* renderer)
{
    g_recordings.clear();
    g_recCommands.clear();
    g_recIndex.clear();
    if (!g_shadowRec || *reinterpret_cast<const int*>(kShadowPrepGlobal) == 0)
        return;
    const uint8_t* list = renderer + kRdLevelLights + 16;
    auto** lights = *reinterpret_cast<uint8_t** const*>(list);
    const int count = *reinterpret_cast<const int*>(list + 8);
    void* sys = *reinterpret_cast<void* const*>(kRenderSystemGlobal);
    if (!lights || !sys)
        return;
    using MatrixFn = const float*(__fastcall*)(void*);
    const float* cam = reinterpret_cast<MatrixFn>((*static_cast<void***>(sys))[0x280 / 8])(sys);
    const float eye[3] = {cam[3], cam[7], cam[11]};
    const int savedLevel = g_shadowLevel;
    const bool savedWindow = g_fillWindow;
    g_shadowLevel = savedLevel + 1; // PrepareDepthMap's EF_StartEf
    g_fillWindow = true;
    g_recordingNow = true;
    for (int i = 0; i < count; i++)
    {
        uint8_t* light = lights[i];
        if (!light)
            continue;
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(light + kLightFlags);
        if (!(flags & 0x10) || (*reinterpret_cast<const int*>(kShadowStaticGlobal) != 0 && (flags & 2)))
            continue;
        auto** frustums = *reinterpret_cast<uint8_t** const*>(light + kLightFrustums);
        for (int k = 0; frustums && k < 8 && frustums[k]; k++)
        {
            uint8_t* frustum = frustums[k];
            const uint8_t* casters = *reinterpret_cast<const uint8_t* const*>(frustum + kFrustumCasters);
            if (!casters)
                continue;
            auto** data = *reinterpret_cast<uint8_t** const*>(casters);
            const int n = *reinterpret_cast<const int*>(casters + 8);
            for (int c = 0; c < n && data; c++)
            {
                uint8_t* caster = data[c];
                if (!caster)
                    continue;
                float box[6] = {};
                using BoxFn = void(__fastcall*)(void*, float*);
                reinterpret_cast<BoxFn>((*reinterpret_cast<void***>(caster))[2])(caster, box);
                float d2 = 0;
                for (int j = 0; j < 3; j++)
                    if (eye[j] < box[j])
                        d2 += (box[j] - eye[j]) * (box[j] - eye[j]);
                    else if (eye[j] > box[3 + j])
                        d2 += (eye[j] - box[3 + j]) * (eye[j] - box[3 + j]);
                alignas(16) uint8_t rp[200] = {};
                const float one = 1.0f;
                const int technique = 4, renderList = 2, afterWater = 1;
                const int objFlags = 0xF | ((*reinterpret_cast<const uint32_t*>(caster + 0xC) & 0x200) ? 0x200 : 0);
                const float distance = sqrtf(d2);
                memcpy(rp + 0x1C, &technique, 4);
                memcpy(rp + 0x38, &one, 4);
                memcpy(rp + 0x3C, &renderList, 4);
                memcpy(rp + 0x50, &distance, 4);
                memcpy(rp + 0x54, &objFlags, 4);
                memcpy(rp + 0x78, &caster, 8);
                memcpy(rp + 0x80, &one, 4);
                rp[0xA0] = static_cast<uint8_t>(afterWater);
                const uint32_t first = static_cast<uint32_t>(g_recCommands.size());
                using RenderFn = void(__fastcall*)(void*, void*);
                reinterpret_cast<RenderFn>((*reinterpret_cast<void***>(caster))[1])(caster, rp);
                g_recIndex[RecKey(reinterpret_cast<uintptr_t>(caster), reinterpret_cast<uintptr_t>(frustum))] = static_cast<uint32_t>(g_recordings.size());
                g_recordings.push_back({reinterpret_cast<uintptr_t>(caster), reinterpret_cast<uintptr_t>(frustum), first,
                                        static_cast<uint32_t>(g_recCommands.size() - first), distance});
                g_recCasters++;
            }
        }
    }
    g_recordingNow = false;
    g_fillWindow = savedWindow;
    g_shadowLevel = savedLevel;
}

// Load overlay (CRYMT_OVERLAY=1; Ctrl+F11 toggles): frame time, work and waits of main and the render thread, CPU use
// of the busiest game threads and of every logical core, refreshed twice a second, drawn with Draw2dLabel.
struct OverlayThread
{
    DWORD tid;
    uint64_t cpu; // 100 ns units, user + kernel
};
void PatchDeviceVtable(void* device);

struct Overlay
{
    bool on;
    bool keyWasDown;
    int64_t lastTick;
    uint64_t lastFrame, lastMainWait, lastBarrierWait, lastRtBusy;
    std::vector<OverlayThread> threads;
    std::vector<uint64_t> coreIdle, coreTotal;
    char lines[6][320];
    int lineCount;
};
Overlay g_overlay;

using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, void*, ULONG, ULONG*);
using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);

// The module a thread was started in (its Win32 start address), for a short label.
void ThreadModule(HANDLE h, char* out, size_t size)
{
    static auto query = reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
    void* start = nullptr;
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "?";
    if (query && query(h, 9, &start, sizeof(start), nullptr) == 0 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(start), &mod))
        GetModuleFileNameA(mod, path, sizeof(path));
    const char* base = strrchr(path, '\\');
    snprintf(out, size, "%s", base ? base + 1 : path);
    if (char* dot = strrchr(out, '.'))
        *dot = 0;
}

void RefreshOverlay()
{
    Overlay& o = g_overlay;
    const int64_t now = Now();
    const double ms = (now - o.lastTick) * g_qpcToMs;
    const uint64_t frames = g_frame - o.lastFrame;
    const uint64_t rtBusy = g_rtBusyTicks.load(std::memory_order_relaxed);
    if (o.lastTick && frames)
    {
        const double f = static_cast<double>(frames);
        snprintf(o.lines[0], sizeof(o.lines[0]), "CryMT  %.0f FPS  frame %.2f ms", f * 1000.0 / ms, ms / f);
        const double barrier = (g_preWaitTicks - o.lastBarrierWait) * g_qpcToMs / f;
        const double syncs = (g_mainWaitTicks - o.lastMainWait) * g_qpcToMs / f;
        snprintf(o.lines[1], sizeof(o.lines[1]), "main    work %.2f ms   waits for render thread %.2f ms (shadow barrier %.2f, syncs %.2f)",
                 ms / f - barrier - syncs, barrier + syncs, barrier, syncs);
        const double busy = (rtBusy - o.lastRtBusy) * g_qpcToMs / f;
        snprintf(o.lines[2], sizeof(o.lines[2]), "render  work %.2f ms   idle %.2f ms", busy, std::max(0.0, ms / f - busy));
    }
    // Threads of this process: CPU in the last interval, as % of one core.
    std::vector<OverlayThread> threads;
    struct Top
    {
        double pct;
        char name[48];
    } top[5] = {};
    double total = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h)
            continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u))
        {
            const uint64_t cpu = (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                                 (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
            threads.push_back({te.th32ThreadID, cpu});
            for (const OverlayThread& old : o.threads)
                if (old.tid == te.th32ThreadID && o.lastTick)
                {
                    const double pct = (cpu - old.cpu) / (ms * 100.0); // 100 ns units over ms * 10000, in %
                    total += pct;
                    Top t{pct, {}};
                    if (te.th32ThreadID == g_mainThread)
                        snprintf(t.name, sizeof(t.name), "main");
                    else if (te.th32ThreadID == g_rtTid)
                        snprintf(t.name, sizeof(t.name), "render");
                    else
                        ThreadModule(h, t.name, sizeof(t.name));
                    for (int i = 0; i < 5; i++)
                        if (t.pct > top[i].pct)
                        {
                            std::swap(t, top[i]);
                        }
                    break;
                }
        }
        CloseHandle(h);
    }
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
    o.threads = std::move(threads);
    int n = snprintf(o.lines[3], sizeof(o.lines[3]), "threads %.0f%% of a core in all:", total);
    for (const Top& t : top)
        if (t.pct >= 1.0 && n < static_cast<int>(sizeof(o.lines[3])) - 40)
            n += snprintf(o.lines[3] + n, sizeof(o.lines[3]) - n, "  %s %.0f%%", t.name, t.pct);
    // Logical cores: busy share since the last refresh (SystemProcessorPerformanceInformation).
    struct CorePerf
    {
        LARGE_INTEGER idle, kernel, user, reserved[2];
        ULONG reserved2;
    };
    static auto querySys = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation"));
    CorePerf perf[128];
    ULONG got = 0;
    o.lineCount = 4;
    if (querySys && querySys(8, perf, sizeof(perf), &got) == 0)
    {
        const size_t cores = got / sizeof(CorePerf);
        const bool have = o.coreIdle.size() == cores;
        o.coreIdle.resize(cores);
        o.coreTotal.resize(cores);
        int line = 4;
        int len = snprintf(o.lines[line], sizeof(o.lines[line]), "cores %%:");
        for (size_t i = 0; i < cores; i++)
        {
            const uint64_t idle = perf[i].idle.QuadPart;
            const uint64_t tot = perf[i].kernel.QuadPart + perf[i].user.QuadPart; // kernel includes idle
            if (have && tot > o.coreTotal[i])
            {
                const double busy = 100.0 * (1.0 - static_cast<double>(idle - o.coreIdle[i]) / (tot - o.coreTotal[i]));
                if (i == cores / 2 && cores > 16 && line == 4)
                {
                    line = 5;
                    len = snprintf(o.lines[line], sizeof(o.lines[line]), "        ");
                }
                len += snprintf(o.lines[line] + len, sizeof(o.lines[line]) - len, " %3.0f", std::max(0.0, busy));
            }
            o.coreIdle[i] = idle;
            o.coreTotal[i] = tot;
        }
        o.lineCount = line + 1;
    }
    o.lastTick = now;
    o.lastFrame = g_frame;
    o.lastMainWait = g_mainWaitTicks;
    o.lastBarrierWait = g_preWaitTicks;
    o.lastRtBusy = rtBusy;
}

void DrawOverlay(void* renderer)
{
    Overlay& o = g_overlay;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (down && !o.keyWasDown)
        o.on = !o.on;
    o.keyWasDown = down;
    if (!o.on)
        return;
    if ((Now() - o.lastTick) * g_qpcToMs >= 500.0 || !o.lastTick)
        RefreshOverlay();
    using Draw2dLabelFn = void (*)(void*, float, float, float, const float*, bool, const char*, ...);
    const auto draw = reinterpret_cast<Draw2dLabelFn>((*static_cast<void***>(renderer))[kDraw2dLabel]);
    static const float kColor[4] = {0.4f, 1.0f, 0.4f, 1.0f};
    for (int i = 0; i < o.lineCount; i++)
        if (o.lines[i][0])
            draw(renderer, 10.0f, 120.0f + 20.0f * i, 1.7f, kColor, false, "%s", o.lines[i]);
}

void* __fastcall HookEndEf3D(void* self, void* a, void* b, void* c)
{
    g_endEfCalls++;
    g_lastEndEfRet = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    // EF_EndEf3D renders some passes (water reflections) by calling back into RenderScene, so the
    // render thread reaches this hook from the same call site; only the main thread starts a frame.
    if (g_mainThread && GetCurrentThreadId() != g_mainThread)
    {
        if (g_mode == 2)
        {
            if (g_lastEndEfRet == kRenderSceneRet && g_preDoneSeq.load(std::memory_order_relaxed) == g_rtFrameSeq)
                g_lateCallbacks++;
            const bool shadowDraw = g_lastEndEfRet - kPrepareDepthMapFn < kPrepareDepthMapSize;
            const int64_t start = shadowDraw ? Now() : 0;
            g_rtDepth++;
            void* r = reinterpret_cast<EndEfFn>(g_orig[kEndEf3DSlot])(self, a, b, c);
            g_rtDepth--;
            if (shadowDraw)
            {
                g_shadowDrawTicks += Now() - start;
                g_shadowDraws++;
            }
            return r;
        }
        if (g_mode != 1)
            return reinterpret_cast<EndEfFn>(g_orig[kEndEf3DSlot])(self, a, b, c);
        void* frames[kNestedDepth + 1];
        const int depth = RtlCaptureStackBackTrace(1, kNestedDepth, frames, nullptr);
        const int64_t start = Now();
        void* r = reinterpret_cast<EndEfFn>(g_orig[kEndEf3DSlot])(self, a, b, c);
        const int64_t end = Now();
        g_frameLastNestedEnd = end;
        int i = 0;
        while (i < g_nestedCount && (g_nested[i].depth != depth || memcmp(g_nested[i].frames, frames, depth * sizeof(void*)) != 0))
            i++;
        if (i == g_nestedCount && g_nestedCount < kMaxNested)
        {
            memcpy(g_nested[i].frames, frames, depth * sizeof(void*));
            g_nested[i].depth = depth;
            g_nestedCount++;
        }
        if (i < g_nestedCount)
        {
            g_nested[i].count++;
            g_nested[i].startTicks += start - g_topStart;
            g_nested[i].durTicks += end - start;
        }
        return r;
    }
    const bool top = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == kRenderSceneRet && g_depth == 0;
    if (!top)
    {
        if (g_depth == 0 && g_mainThread)
        {
            g_otherEndEf++;
            if (g_mode == 2)
                WaitIdle(kEndEf3DSlot);
        }
        g_depth++;
        void* r = reinterpret_cast<EndEfFn>(g_orig[kEndEf3DSlot])(self, a, b, c);
        g_depth--;
        return r;
    }

    g_endEfTop++;
    CensusEnd();
    g_fillWindow = false;
    if (!g_mainThread)
        g_mainThread = GetCurrentThreadId();
    g_renderer = reinterpret_cast<uintptr_t>(self);
    const int64_t start = Now();
    if (g_inWindow)
        g_windowSum += start - g_windowStart;
    g_inWindow = false;
    if (g_mode != 2 && g_frame == g_traceFrame)
        WriteTrace();

    void* r = nullptr;
    if (g_mode == 2)
    {
        StartRenderThread();
        DrawOverlay(self);
        D3DLayerFrame(*reinterpret_cast<void**>(static_cast<uint8_t*>(self) + kDeviceOffset));
        // CRYMT_DEVGATE: count device calls made by main while the render thread runs, and by other threads.
        static const bool devGate = GetEnvironmentVariableW(L"CRYMT_DEVGATE", nullptr, 0) != 0;
        if (devGate && !g_devSlotCount)
            if (void* device = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + kDeviceOffset))
                PatchDeviceVtable(device);
        const uint64_t waitBeforeEnd = g_mainWaitTicks;
        WaitIdle(295);
        FrameDetail* detail = g_frame < kMaxDetail ? &g_detail[g_frame] : nullptr;
        if (detail)
        {
            detail->waitEnd = static_cast<float>((g_mainWaitTicks - waitBeforeEnd) * g_qpcToMs);
            detail->waitOther = static_cast<float>((waitBeforeEnd - g_waitAtFrameStart) * g_qpcToMs);
            const ReasonWait* top = nullptr;
            for (int i = 0; i < g_frameWaitCount; i++)
                if (!top || g_frameWaits[i].ticks > top->ticks)
                    top = &g_frameWaits[i];
            detail->topReason = top ? top->reason : 0;
            detail->topWait = top ? static_cast<float>(top->ticks * g_qpcToMs) : 0.0f;
        }
        g_frameWaitCount = 0;
        if (g_terrainOwn)
        {
            // The render thread is idle here: the frame's meshes become its own, and the element pool is refilled.
            g_mainMeshes.clear();
            using CreateREFn = void*(__fastcall*)(void*, int);
            while (g_rePool.size() < kRePoolSize)
            {
                void* re = reinterpret_cast<CreateREFn>(g_orig[kCreateRE])(self, kRETypeMesh);
                if (!re)
                    break;
                g_rePool.push_back(re);
            }
        }
        RunPendingMaterialDeletes(); // the previous flush is done (WaitIdle 295)
        SnapshotTimer();
        if (g_fillOn)
        {
            CopyRendererToFake(static_cast<const uint8_t*>(self));
            RunPendingGc();
            TrackEngineLocks();
        }
        if (g_fillOn)
            RecordShadowCasters(static_cast<uint8_t*>(self)); // before the pose capture: casters add characters
        if (g_frame >= g_warmupFrames)
            PrepareSkinning(reinterpret_cast<uintptr_t>(self));
        else
            g_frameObjects.clear();
        Command& cmd = BeginCommand(kEndEf3DSlot);
        cmd.regs = {};
        cmd.regs.rcx = reinterpret_cast<uint64_t>(self);
        cmd.regs.rdx = reinterpret_cast<uint64_t>(a);
        cmd.regs.r8 = reinterpret_cast<uint64_t>(b);
        cmd.regs.r9 = reinterpret_cast<uint64_t>(c);
        memset(cmd.stack, 0, sizeof(cmd.stack));
        cmd.count = static_cast<uint32_t>(++g_frameSeq);
        g_flushCmdIndex = g_write.load(std::memory_order_relaxed);
        PublishCommand();
        // Hold main until the flush has finished its engine callbacks (or the whole flush, if the
        // preprocess call was skipped this frame).
        const int64_t waitStart = Now();
        for (uint32_t spin = 0;; spin++)
        {
            if (g_preDoneSeq.load(std::memory_order_acquire) == g_frameSeq)
                break;
            if (g_read.load(std::memory_order_acquire) > g_flushCmdIndex)
            {
                g_preFallbacks++;
                break;
            }
            _mm_pause();
            if ((spin & 0xFFF) == 0xFFF)
                PumpSentMessages();
        }
        g_preWaitTicks += Now() - waitStart;
        if (detail)
        {
            detail->waitBarrier = static_cast<float>((Now() - waitStart) * g_qpcToMs);
            detail->seq = static_cast<uint32_t>(g_frameSeq);
        }
        g_snapInvalid = false;
        g_snapMatrixInvalid = false;
        g_matrixDepth = 0;
        g_shadowLevel--;
        if (g_frame < g_warmupFrames || g_levelChange || g_frame < g_syncUntilFrame)
            WaitIdle(296);
        g_firstSyncSeen = false;
        g_dispatchTime = start;
    }
    else
    {
        g_depth = 1;
        r = g_mode == 1 ? CallEndEfOnWorker(self, a, b, c) : reinterpret_cast<EndEfFn>(g_orig[kEndEf3DSlot])(self, a, b, c);
        g_depth = 0;
    }
    const int64_t end = Now();
    g_renderSum += end - start;
    if (g_frame < kMaxFrameLog)
        g_frameLog[g_frame] = start;
    g_frame++;
    g_windowStart = end;
    g_inWindow = true;
    if (g_frame % 500 == 0 || g_frame == 1 || g_frame == 30 || g_frame == 100)
        WriteStats();
    // The logs are rewritten whole, which takes long once a session has many frames: only on request.
    if (g_frameLogOn && g_frame % 3000 == 0)
    {
        WriteFrameLog();
        if (g_mode == 2)
        {
            WriteDetailLog();
            WriteGpuLog();
        }
    }
    g_waitAtFrameStart = g_mainWaitTicks;
    return r;
}

void SetPolicies()
{
    if (g_level < 1)
        return;
    for (int s : {2, 7, 9, 58, 59, 98, 122, 123, 134, 135, 174, 199, 201})
        g_policy[s] = kDirect;
    if (g_level < 2)
        return;
    for (int s : {kRenderDebug, kEndFrame, kSetViewport, kSetTexture, kDraw2dImage, kFontSetTexture, kFontSetState,
                  kFontSetBlending, kFontRestoreState, kSetClearColor, kDeleteRenderMesh, kSetStateWrap, kSetCullMode,
                  kSetColorOp, kEnableTMU, kSelectTMU, kResetToDefault, kSetMaterialColor, kFlushTextMessages,
                  kSet2DMode, kSetState})
        g_policy[s] = kDefer;
    for (int s : {kBeginFrame, kGetDynVBPtr, kDrawDynVB, kEfQuery, kGetFrameID, kSwapFogFlag, kDraw2dLabel, kGetViewport,
                  kGetCamera, kProjectToScreen, kUnProjectFromScreen, kGetModelView, kGetProjection})
        g_policy[s] = kSpecial;
    g_policy[kGetIRenderAuxGeom] = kDirect;
    for (int s = 0; s < kAuxSlots; s++)
        if (kAuxSpecs[s].copy || s == kAuxGetFlags || s == kAuxNoOp || s == kAuxFlush)
            g_policy[kAuxBase + s] = kSpecial;
}

} // namespace

// PreThunk swaps the return address of the patched call so the callee sees the caller's exact stack
// frame; PrePost runs when it returns. Return addresses are kept per thread because the preprocess
// stage re-enters itself through nested flushes.
struct PreStack
{
    uintptr_t ret[64];
    int depth;
};
DWORD g_preTls = TLS_OUT_OF_INDEXES;

extern "C" void PrePush(uintptr_t ret)
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_preTls));
    if (!s)
    {
        s = static_cast<PreStack*>(calloc(1, sizeof(PreStack)));
        TlsSetValue(g_preTls, s);
    }
    if (s->depth == 0 && GetCurrentThreadId() == g_rtTid && g_rtDepth == 0)
    {
        g_phaseMark = Now();
        g_phaseTicks[0] += g_phaseMark - g_flushStartTick;
    }
    s->ret[s->depth++] = ret;
}

// Shadow preparation (FUN_380BEE30, called at 0x380AA636 right after preprocess) also calls back into
// Cry3DEngine to draw terrain sectors into shadow maps. FUN_380AA200 skips it when the global below is
// zero or bit 8 of its flags (r13) is set; then the signal is raised after preprocess instead.
DWORD g_shadowTls = TLS_OUT_OF_INDEXES;
bool g_shadowBarrier = true; // main waits until shadow prep is done unless CRYMT_EARLYBARRIER is set

void CaptureRtPoses();

// CRYMT_FRUSTUMTEST: which bytes of the level-1 lights (CDLight, 0x258) and their shadow frustums (0x388,
// array of up to 8 at light+0x1F0) the render thread changes after the engine callbacks, i.e. during S.
// Renderer light list for level L: TArray at renderer + (L + 0xF4) * 16 (pointer, count).
struct ProbeCopy
{
    const uint8_t* live;
    size_t bytes;
    size_t at;
};
std::vector<ProbeCopy> g_probe;
std::vector<uint8_t> g_probeData;
uint64_t g_lightChangedP[kLightBytes]; // during preprocess and shadow prep (P)
uint64_t g_frustumChangedP[kFrustumBytes];
uint64_t g_lightChanged[kLightBytes]; // after the engine callbacks (S)
uint64_t g_frustumChanged[kFrustumBytes];
uint64_t g_probeFrames;

void ProbeSnapshot()
{
    g_probe.clear();
    g_probeData.clear();
    const auto* list = reinterpret_cast<const uint8_t*>(g_renderer) + (1 + 0xF4) * 16;
    auto* const* lights = *reinterpret_cast<uint8_t* const* const*>(list);
    const int count = *reinterpret_cast<const int*>(list + 8);
    const auto add = [](const uint8_t* p, size_t n) {
        g_probe.push_back({p, n, g_probeData.size()});
        g_probeData.insert(g_probeData.end(), p, p + n);
    };
    for (int i = 0; i < count && lights; i++)
    {
        const uint8_t* light = lights[i];
        if (!light)
            continue;
        add(light, kLightBytes);
        auto* const* frustums = *reinterpret_cast<uint8_t* const* const*>(light + 0x1F0);
        for (int k = 0; frustums && k < 8 && frustums[k]; k++)
            add(frustums[k], kFrustumBytes);
    }
}

void ProbeCompare(bool afterP)
{
    for (const ProbeCopy& p : g_probe)
    {
        uint64_t* hist = p.bytes == kLightBytes ? (afterP ? g_lightChangedP : g_lightChanged) : (afterP ? g_frustumChangedP : g_frustumChanged);
        for (size_t b = 0; b < p.bytes; b++)
            if (p.live[b] != g_probeData[p.at + b])
                hist[b]++;
    }
    if (!afterP)
        g_probeFrames++;
    g_probe.clear();
}

void ProbeCompare()
{
    ProbeCompare(false);
}

// Flush start: the level-1 light list was filled by main's frame.
void ProbeFlushStart()
{
    if (g_frustumTest)
        ProbeSnapshot();
}

void WriteProbe(FILE* f)
{
    if (!g_frustumTest)
        return;
    fprintf(f, "frustum test over %llu flushes, offsets the render thread changed during S (offset x frames):\n  lights:",
            static_cast<unsigned long long>(g_probeFrames));
    for (size_t b = 0; b < kLightBytes; b++)
        if (g_lightChanged[b])
            fprintf(f, " %zx x%llu", b, static_cast<unsigned long long>(g_lightChanged[b]));
    fprintf(f, "\n  frustums:");
    for (size_t b = 0; b < kFrustumBytes; b++)
        if (g_frustumChanged[b])
            fprintf(f, " %zx x%llu", b, static_cast<unsigned long long>(g_frustumChanged[b]));
    fprintf(f, "\n  during P, lights:");
    for (size_t b = 0; b < kLightBytes; b++)
        if (g_lightChangedP[b])
            fprintf(f, " %zx x%llu", b, static_cast<unsigned long long>(g_lightChangedP[b]));
    fprintf(f, "\n  during P, frustums:");
    for (size_t b = 0; b < kFrustumBytes; b++)
        if (g_frustumChangedP[b])
            fprintf(f, " %zx x%llu", b, static_cast<unsigned long long>(g_frustumChangedP[b]));
    fprintf(f, "\n");
}

// Fill mode: the flush works on copies of the level-1 lights and their shadow frustums, so main can rebuild the
// 3D engine's per-frame light array (C3DEngine+0x760, CDLight by value) and its frustums (InitShadowFrustums)
// for the next frame while the flush still draws this one. Measured with CRYMT_FRUSTUMTEST: during preprocess
// and shadow prep the renderer writes many frustum bytes (matrices 0x00..0x7F, update masks at +0x86, +0xC8),
// afterwards only frustum +0xC0 and light scissor shorts (+0x6E/+0x72/+0x76). So the bytes the flush changed are
// carried back to the originals: before main is released for P (main is held), at the end of the flush for the
// frustums (S), while the lights' S changes are per-frame scratch and dropped.
struct FlushCopy
{
    uint8_t* orig;
    uint8_t* copy;
    uint8_t* base; // copy contents at the start of the current phase
    size_t bytes;
};
std::vector<FlushCopy> g_flushCopies;
uint8_t* g_flushMem;
size_t g_flushUsed;
constexpr size_t kFlushMemBytes = 4u << 20;

uint8_t* FlushAlloc(size_t bytes)
{
    bytes = (bytes + 15) & ~size_t{15};
    if (!g_flushMem)
        g_flushMem = static_cast<uint8_t*>(VirtualAlloc(nullptr, kFlushMemBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_flushMem || g_flushUsed + bytes > kFlushMemBytes)
        return nullptr;
    uint8_t* p = g_flushMem + g_flushUsed;
    g_flushUsed += bytes;
    return p;
}

uint8_t* FlushCopyOf(uint8_t* orig, size_t bytes)
{
    uint8_t* copy = FlushAlloc(bytes);
    uint8_t* base = FlushAlloc(bytes);
    if (!copy || !base)
        return nullptr;
    memcpy(copy, orig, bytes);
    memcpy(base, orig, bytes);
    g_flushCopies.push_back({orig, copy, base, bytes});
    return copy;
}

// Flush start, render thread, main held.
void FlushLightsBegin()
{
    g_flushCopies.clear();
    g_flushUsed = 0;
    if (!g_fillOn)
        return;
    auto* list = reinterpret_cast<uint8_t*>(g_renderer) + (1 + 0xF4) * 16;
    auto** lights = *reinterpret_cast<uint8_t***>(list);
    const int count = *reinterpret_cast<const int*>(list + 8);
    for (int i = 0; i < count && lights; i++)
    {
        uint8_t* light = lights[i];
        if (!light)
            continue;
        uint8_t* lightCopy = FlushCopyOf(light, kLightBytes);
        if (!lightCopy)
            return;
        auto** frustums = *reinterpret_cast<uint8_t***>(light + 0x1F0);
        if (frustums)
        {
            auto** array = reinterpret_cast<uint8_t**>(FlushAlloc(8 * sizeof(void*)));
            if (!array)
                return;
            for (int k = 0; k < 8; k++)
                array[k] = frustums[k] ? FlushCopyOf(frustums[k], kFrustumBytes) : nullptr;
            *reinterpret_cast<uint8_t***>(lightCopy + 0x1F0) = array;
        }
        lights[i] = lightCopy;
        g_flushLightsCopied++;
    }
}

void CarryBack(bool frustumsOnly)
{
    for (FlushCopy& c : g_flushCopies)
    {
        if (frustumsOnly && c.bytes != kFrustumBytes)
            continue;
        for (size_t b = 0; b < c.bytes; b++)
            if (c.copy[b] != c.base[b] && !(c.bytes == kLightBytes && b >= 0x1F0 && b < 0x1F8))
                c.orig[b] = c.copy[b];
        memcpy(c.base, c.copy, c.bytes);
    }
}

void SignalEngineCallbacksDone()
{
    CarryBack(false);
    CaptureRtPoses();
    if (g_frustumTest)
    {
        ProbeCompare(true);
        ProbeSnapshot();
    }
    g_preSignals++;
    g_preDoneTime = Now();
    g_preDoneSeq.store(g_rtFrameSeq, std::memory_order_release);
}

extern "C" uintptr_t PrePop(uint64_t r13)
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_preTls));
    const uintptr_t ret = s->ret[--s->depth];
    if (s->depth == 0 && GetCurrentThreadId() == g_rtTid && g_rtDepth == 0)
    {
        g_phaseTicks[1] += Now() - g_phaseMark;
        // Shadow prep only calls back into the engine for terrain sectors; with the terrain calls of the
        // next frame's C3DEngine::Update fenced, main may continue as soon as preprocess is done.
        const bool shadowsSkipped = *reinterpret_cast<const int*>(kShadowPrepGlobal) == 0 || (r13 & 8) != 0;
        if (shadowsSkipped || !g_shadowBarrier)
            SignalEngineCallbacksDone();
    }
    return ret;
}

extern "C" void ShadowPush(uintptr_t ret)
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_shadowTls));
    if (!s)
    {
        s = static_cast<PreStack*>(calloc(1, sizeof(PreStack)));
        TlsSetValue(g_shadowTls, s);
    }
    if (s->depth == 0 && GetCurrentThreadId() == g_rtTid && g_rtDepth == 0)
        g_phaseMark = Now();
    s->ret[s->depth++] = ret;
}

extern "C" uintptr_t ShadowPop()
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_shadowTls));
    const uintptr_t ret = s->ret[--s->depth];
    if (s->depth == 0 && GetCurrentThreadId() == g_rtTid && g_rtDepth == 0)
        g_phaseTicks[2] += Now() - g_phaseMark;
    if (s->depth == 0 && GetCurrentThreadId() == g_rtTid && g_rtDepth == 0 && g_shadowBarrier)
        SignalEngineCallbacksDone();
    return ret;
}

extern "C" void ShadowThunk();
asm(R"(
.intel_syntax noprefix
.text
.globl ShadowThunk
ShadowThunk:
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 0x68
    movups [rsp+0x20], xmm0
    movups [rsp+0x30], xmm1
    movups [rsp+0x40], xmm2
    movups [rsp+0x50], xmm3
    mov rcx, [rsp+0x88]
    call ShadowPush
    movups xmm0, [rsp+0x20]
    movups xmm1, [rsp+0x30]
    movups xmm2, [rsp+0x40]
    movups xmm3, [rsp+0x50]
    add rsp, 0x68
    pop r9
    pop r8
    pop rdx
    pop rcx
    lea rax, [rip+ShadowPost]
    mov [rsp], rax
    movabs rax, 0x380BEE30
    jmp rax
ShadowPost:
    sub rsp, 0x40
    mov [rsp+0x20], rax
    movups [rsp+0x28], xmm0
    call ShadowPop
    mov r11, rax
    mov rax, [rsp+0x20]
    movups xmm0, [rsp+0x28]
    add rsp, 0x40
    jmp r11
.att_syntax prefix
)");

// Same return-address swap for C3DEngine::RenderScene (vtable slot 0x5B8/8). With CRYMT_SCENESYNC the
// main thread waits for the render thread as soon as RenderScene returns.
DWORD g_sceneTls = TLS_OUT_OF_INDEXES;
bool g_sceneSync;
void WaitIdlePublic(int reason);

// End-of-RenderScene garbage collection frees data the in-flight flush still draws. The calls are
// recorded and run on main at the start of the next RenderScene, after the render thread is idle.
void* g_pendingTerrain;
void* g_pendingTmpPool;
bool g_pendingTmpPoolAll;
uint64_t g_gcDeferred, g_gcDropped;

// The renderer draws characters through CRenderObject::m_pCharInstance (ISkinnable) without holding a
// reference. Releases made by main while a flush is in flight are queued until the render thread is idle.
// Character operations that can destroy data the flush still skins are queued in order:
// Release (slot 1), the scalar deleting destructor (slot 54) and CSkinInstance slot 30, which clears
// the model pointer at +0xE8 that GetSkeletonPose reads.
constexpr int kReleaseSlot = 1;
constexpr int kClearModelSlot = 30;
constexpr int kDeletingDtorSlot = 54;
uintptr_t g_charDeletingDtor;
uintptr_t g_skinDeletingDtor;
uintptr_t g_skinClearModel;
struct PendingOp
{
    uintptr_t fn;
    void* self;
    uint64_t arg;
};
std::vector<PendingOp> g_pendingReleases;
uint64_t g_releaseDeferred;

bool DeferCharacterOpsNow()
{
    return g_mode == 2 && g_async && GetCurrentThreadId() == g_mainThread;
}

extern "C" void __fastcall DeferredSkinRelease(void* self)
{
    if (DeferCharacterOpsNow())
    {
        g_pendingReleases.push_back({kSkinRelease, self, 0});
        g_releaseDeferred++;
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(kSkinRelease)(self);
}

extern "C" void* __fastcall DeferredCharDelete(void* self, uint64_t flags)
{
    if (DeferCharacterOpsNow())
    {
        g_pendingReleases.push_back({g_charDeletingDtor, self, flags});
        g_releaseDeferred++;
        return self;
    }
    return reinterpret_cast<void*(__fastcall*)(void*, uint64_t)>(g_charDeletingDtor)(self, flags);
}

extern "C" void* __fastcall DeferredSkinDelete(void* self, uint64_t flags)
{
    if (DeferCharacterOpsNow())
    {
        g_pendingReleases.push_back({g_skinDeletingDtor, self, flags});
        g_releaseDeferred++;
        return self;
    }
    return reinterpret_cast<void*(__fastcall*)(void*, uint64_t)>(g_skinDeletingDtor)(self, flags);
}

uintptr_t g_charClearModel;

// Render-thread frame time (CryEngine 3/5 keep one per render frame). Main advances CTimer at the start
// of the next frame (UpdateOnFrameStart, slot 1) while the flush still runs, so time-dependent shaders
// such as vegetation bending got the next frame's time. Getters called from the render thread return
// values captured when the frame was queued. Slot order matches ITimer.h of the Mod SDK.
uintptr_t g_timerOrig[13];
void* g_timer;
float g_tCurr[2], g_tFrame[2], g_tReal, g_tScale, g_tRate;
int64_t g_tStart[2];
bool g_timerSnap;

extern "C" void __fastcall TimerUpdateOnFrameStart(void* t)
{
    g_timer = t;
    reinterpret_cast<void(__fastcall*)(void*)>(g_timerOrig[1])(t);
}
extern "C" float __fastcall TimerGetCurrTime(void* t, int which)
{
    if (g_timerSnap && GetCurrentThreadId() == g_rtTid && (which == 0 || which == 1))
        return g_tCurr[which];
    return reinterpret_cast<float(__fastcall*)(void*, int)>(g_timerOrig[2])(t, which);
}
extern "C" int64_t* __fastcall TimerGetFrameStartTime(void* t, int64_t* out, int which)
{
    if (g_timerSnap && GetCurrentThreadId() == g_rtTid && (which == 0 || which == 1))
    {
        *out = g_tStart[which];
        return out;
    }
    return reinterpret_cast<int64_t*(__fastcall*)(void*, int64_t*, int)>(g_timerOrig[3])(t, out, which);
}
extern "C" float __fastcall TimerGetFrameTime(void* t, int which)
{
    if (g_timerSnap && GetCurrentThreadId() == g_rtTid && (which == 0 || which == 1))
        return g_tFrame[which];
    return reinterpret_cast<float(__fastcall*)(void*, int)>(g_timerOrig[6])(t, which);
}
extern "C" float __fastcall TimerGetRealFrameTime(void* t)
{
    if (g_timerSnap && GetCurrentThreadId() == g_rtTid)
        return g_tReal;
    return reinterpret_cast<float(__fastcall*)(void*)>(g_timerOrig[7])(t);
}
extern "C" float __fastcall TimerGetTimeScale(void* t)
{
    if (g_timerSnap && GetCurrentThreadId() == g_rtTid)
        return g_tScale;
    return reinterpret_cast<float(__fastcall*)(void*)>(g_timerOrig[8])(t);
}
extern "C" float __fastcall TimerGetFrameRate(void* t)
{
    if (g_timerSnap && GetCurrentThreadId() == g_rtTid)
        return g_tRate;
    return reinterpret_cast<float(__fastcall*)(void*)>(g_timerOrig[12])(t);
}

// Main thread, render thread idle: capture the time of the frame being queued.
void SnapshotTimer()
{
    if (!g_timer)
        return;
    for (int w = 0; w < 2; w++)
    {
        g_tCurr[w] = reinterpret_cast<float(__fastcall*)(void*, int)>(g_timerOrig[2])(g_timer, w);
        reinterpret_cast<int64_t*(__fastcall*)(void*, int64_t*, int)>(g_timerOrig[3])(g_timer, &g_tStart[w], w);
        g_tFrame[w] = reinterpret_cast<float(__fastcall*)(void*, int)>(g_timerOrig[6])(g_timer, w);
    }
    g_tReal = reinterpret_cast<float(__fastcall*)(void*)>(g_timerOrig[7])(g_timer);
    g_tScale = reinterpret_cast<float(__fastcall*)(void*)>(g_timerOrig[8])(g_timer);
    g_tRate = reinterpret_cast<float(__fastcall*)(void*)>(g_timerOrig[12])(g_timer);
    g_timerSnap = true;
}

// Skinning prepared on main before the flush is queued (what CryEngine 3 did with its skinning data):
// GetSkeletonPose (vtable slot 2) is called for every character of the frame and its bone arrays are
// copied into frame memory; the render thread then gets the copy instead of reading live characters.
constexpr int kSkeletonPoseSlot = 2;
constexpr size_t kQuatTSSize = 32;
constexpr size_t kShapeDeformBytes = 128;
using GetPoseFn = uint32_t(__fastcall*)(void*, int, const void*, void**, void**, void**, void**, float*, uint32_t*, void**);
uintptr_t g_charGetPose;
uintptr_t g_skinGetPose;
struct PoseEntry
{
    void* skin;
    int lod;
    const void* matrix;
    uint32_t bones;
    uint32_t morph;
    void* quats[4];
    void* remap;
    float shape[32];
};
std::vector<PoseEntry> g_poseCache;
std::vector<uintptr_t> g_frameObjects;
std::vector<uintptr_t> g_rtFrameObjects;
uint64_t g_poseHits;
uint64_t g_poseMisses;
uint64_t g_poseMissesLate;
uint64_t g_posePrepared;
LateMiss g_lateMiss[kLateMissLog];

// The input flags ask for the motion-blur bones (4) only inside the object motion blur pass
// (FUN_38065C70 sets bit 24 of +0xAF8 for its duration), which runs later inside the flush. Poses are
// always taken with them, so that pass gets a copy instead of a null array (r_MotionBlur 3+, Very High).
constexpr uint32_t kPoseWantsMotionBlur = 4;
constexpr uint32_t kPoseWantsSkinning = 2;

// Copies of poses taken on the render thread; reset by main with the render thread idle.
constexpr size_t kRtPoseBytes = 8u << 20;
uint8_t* g_rtPoseMem;
size_t g_rtPoseUsed;

void* RtPoseAlloc(size_t bytes)
{
    bytes = (bytes + 15) & ~size_t{15};
    if (!g_rtPoseMem)
        g_rtPoseMem = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRtPoseBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_rtPoseMem || g_rtPoseUsed + bytes > kRtPoseBytes)
        return nullptr;
    void* p = g_rtPoseMem + g_rtPoseUsed;
    g_rtPoseUsed += bytes;
    return p;
}

GetPoseFn OriginalGetPose(void* skin)
{
    const auto vt = *static_cast<uintptr_t*>(skin);
    if (vt == kCharInstanceVtable)
        return reinterpret_cast<GetPoseFn>(g_charGetPose);
    if (vt == kSkinInstanceVtable)
        return reinterpret_cast<GetPoseFn>(g_skinGetPose);
    return reinterpret_cast<GetPoseFn>(reinterpret_cast<uintptr_t*>(vt)[kSkeletonPoseSlot]);
}

// Runs the original GetSkeletonPose for e.skin/e.lod/e.matrix and copies the bone arrays with alloc.
// Only while main cannot touch the character: main with the render thread idle, or the render thread
// while main is held at the preprocess barrier.
bool CapturePose(PoseEntry& e, void* (*alloc)(size_t))
{
    e.morph = kPoseWantsSkinning | kPoseWantsMotionBlur;
    void* live[4] = {};
    e.bones = OriginalGetPose(e.skin)(e.skin, e.lod, e.matrix, &live[0], &live[1], &live[2], &live[3], e.shape, &e.morph, &e.remap);
    // A skin attachment (CSkinInstance) returns the length of its remap table, but the arrays are the
    // master skeleton's and the renderer reads quats[remap[i]] (0x38027180), so copy up to the highest
    // remapped bone. Copying only e.bones left e.g. the first-person right hand reading garbage.
    uint32_t arrayBones = e.bones;
    if (e.remap)
        for (uint32_t i = 0; i < e.bones; i++)
            arrayBones = std::max<uint32_t>(arrayBones, static_cast<const uint8_t*>(e.remap)[i] + 1u);
    for (int k = 0; k < 4; k++)
    {
        if (!live[k] || !e.bones)
        {
            e.quats[k] = live[k];
            continue;
        }
        e.quats[k] = alloc(arrayBones * kQuatTSSize);
        if (!e.quats[k])
            return false;
        memcpy(e.quats[k], live[k], arrayBones * kQuatTSSize);
    }
    return true;
}

uint32_t ReturnPose(const PoseEntry& e, void** l, void** s, void** mbl, void** mbs, float* shape, uint32_t* morph, void** remap)
{
    *l = e.quats[0];
    *s = e.quats[1];
    *mbl = e.quats[2];
    *mbs = e.quats[3];
    memcpy(shape, e.shape, kShapeDeformBytes);
    *morph = e.morph;
    *remap = e.remap;
    return e.bones;
}

uint32_t __fastcall HookGetPose(void* skin, int lod, const void* matrix, void** l, void** s, void** mbl, void** mbs, float* shape,
                                uint32_t* morph, void** remap)
{
    if (GetCurrentThreadId() != g_rtTid)
        return OriginalGetPose(skin)(skin, lod, matrix, l, s, mbl, mbs, shape, morph, remap);
    for (const PoseEntry& e : g_poseCache)
        if (e.skin == skin && e.lod == lod && e.matrix == matrix)
        {
            g_poseHits++;
            return ReturnPose(e, l, s, mbl, mbs, shape, morph, remap);
        }
    g_poseMisses++;
    // Characters first drawn by the flush itself (shadow casters, reflections) while main is still held:
    // take the pose now and keep it for the passes that draw them after main is released.
    if (g_async && g_preDoneSeq.load(std::memory_order_acquire) != g_rtFrameSeq)
    {
        PoseEntry e{};
        e.skin = skin;
        e.lod = lod;
        e.matrix = matrix;
        if (CapturePose(e, RtPoseAlloc))
        {
            g_poseCache.push_back(e);
            return ReturnPose(e, l, s, mbl, mbs, shape, morph, remap);
        }
    }
    else
    {
        // Main is running: another render object of a character already taken this frame gets the same
        // bone arrays (they belong to the skeleton; lod and matrix only steer CPU-side deformation).
        for (const PoseEntry& e : g_poseCache)
            if (e.skin == skin)
            {
                g_poseHits++;
                return ReturnPose(e, l, s, mbl, mbs, shape, morph, remap);
            }
        if (g_poseMissesLate < kLateMissLog)
            g_lateMiss[g_poseMissesLate] = {*static_cast<uintptr_t*>(skin), reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                                            reinterpret_cast<uintptr_t>(matrix) - 0x14, lod};
        g_poseMissesLate++;
    }
    return OriginalGetPose(skin)(skin, lod, matrix, l, s, mbl, mbs, shape, morph, remap);
}

void* ArenaAllocPublic(size_t bytes);

// Takes the poses of the characters on the given render objects that are not cached yet.
void CaptureObjectPoses(std::vector<uintptr_t>& objects, void* (*alloc)(size_t))
{
    for (uintptr_t obj : objects)
    {
        if (IsProxy(obj)) // fill mode: the real object the flush draws (render thread idle here)
            obj = reinterpret_cast<uintptr_t>(reinterpret_cast<ProxyObj*>(obj)->real);
        if (!obj)
            continue;
        void* skin = *reinterpret_cast<void**>(obj + kObjCharInstance);
        if (!skin)
            continue;
        PoseEntry e{};
        e.skin = skin;
        e.lod = *reinterpret_cast<uint8_t*>(obj + kObjLod);
        e.matrix = reinterpret_cast<const void*>(obj + kObjMatrix);
        bool seen = false;
        for (const PoseEntry& c : g_poseCache)
            if (c.skin == e.skin && c.lod == e.lod && c.matrix == e.matrix)
                seen = true;
        if (seen)
            continue;
        if (!CapturePose(e, alloc))
            break;
        g_poseCache.push_back(e);
        g_posePrepared++;
    }
    objects.clear();
}

// Called on main with the render thread idle, right before the frame is queued.
void PrepareSkinning(uintptr_t)
{
    g_poseCache.clear();
    g_rtPoseUsed = 0;
    CaptureObjectPoses(g_frameObjects, ArenaAllocPublic);
}

// Render thread, main still held: characters the flush itself put on render objects (shadow casters
// gathered by shadow prep) are skinned only after main is released, so take their poses now.
void CaptureRtPoses()
{
    if (g_async)
        CaptureObjectPoses(g_rtFrameObjects, RtPoseAlloc);
    else
        g_rtFrameObjects.clear();
}

extern "C" void __fastcall DeferredCharClearModel(void* self)
{
    if (DeferCharacterOpsNow())
    {
        g_pendingReleases.push_back({g_charClearModel, self, 0});
        g_releaseDeferred++;
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(g_charClearModel)(self);
}

extern "C" void __fastcall DeferredSkinClearModel(void* self)
{
    if (DeferCharacterOpsNow())
    {
        g_pendingReleases.push_back({g_skinClearModel, self, 0});
        g_releaseDeferred++;
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(g_skinClearModel)(self);
}

// Level change: unloading frees meshes, materials and the terrain the in-flight flush may still draw, and its leak
// check walked the material list while the render thread deleted a material (null vtable, 0x300E8F9C). Before
// UnloadLevel and LoadLevel the render thread goes idle and the deferred calls run against the old level; frames stay
// synchronous until 30 frames after PostLoadLevel. C3DEngine vtable 0x301D37E8: IProcess slots 0-5, then
// SetLevelPath 6, LoadLevel 7, InitLevelForEditor 8, UnloadLevel 9, PostLoadLevel 10 (Mod SDK order, checked against
// the known Update/RenderWorld/OnFrameStart entries).
using UnloadLevelFn = void(__fastcall*)(void*, bool);
using LoadLevelFn = bool(__fastcall*)(void*, const char*, const char*);
using PostLoadLevelFn = void(__fastcall*)(void*);
UnloadLevelFn g_origUnloadLevel;
LoadLevelFn g_origLoadLevel;
PostLoadLevelFn g_origPostLoadLevel;
uint64_t g_levelChanges;

void BeginLevelChange()
{
    if (g_mode != 2 || GetCurrentThreadId() != g_mainThread)
        return;
    WaitIdlePublic(297);
    RunPendingGc();
    RunPendingMaterialDeletes();
    g_levelChange = true;
}

void __fastcall HookUnloadLevel(void* engine, bool force)
{
    BeginLevelChange();
    g_levelChanges++;
    g_origUnloadLevel(engine, force);
}

bool __fastcall HookLoadLevel(void* engine, const char* folder, const char* mission)
{
    BeginLevelChange();
    return g_origLoadLevel(engine, folder, mission);
}

void __fastcall HookPostLoadLevel(void* engine)
{
    g_origPostLoadLevel(engine);
    if (g_mode == 2 && GetCurrentThreadId() == g_mainThread)
        WaitIdlePublic(298);
    g_levelChange = false;
    g_syncUntilFrame = g_frame + 30;
}

void RunPendingGc()
{
    for (const PendingOp& op : g_pendingReleases)
        reinterpret_cast<void(__fastcall*)(void*, uint64_t)>(op.fn)(op.self, op.arg);
    g_pendingReleases.clear();
    if (g_pendingTerrain)
    {
        // After a level change the recorded terrain is gone (crashed in its node loop, 0x300A7DC4): only the current one.
        if (g_pendingTerrain == *reinterpret_cast<void* const*>(kTerrainGlobal))
            reinterpret_cast<void(__fastcall*)(void*)>(kTerrainUnloadFn)(g_pendingTerrain);
        else
            g_gcDropped++;
        g_pendingTerrain = nullptr;
    }
    if (g_pendingTmpPool)
    {
        reinterpret_cast<void(__fastcall*)(void*, char)>(kTmpPoolFn)(g_pendingTmpPool, g_pendingTmpPoolAll ? 1 : 0);
        g_pendingTmpPool = nullptr;
        g_pendingTmpPoolAll = false;
    }
}

extern "C" void __fastcall DeferTerrainUnload(void* terrain)
{
    if (g_mode == 2 && g_async && GetCurrentThreadId() == g_mainThread)
    {
        g_pendingTerrain = terrain;
        g_gcDeferred++;
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(kTerrainUnloadFn)(terrain);
}

extern "C" void __fastcall DeferTmpPool(void* engine, char all)
{
    if (g_mode == 2 && g_async && GetCurrentThreadId() == g_mainThread)
    {
        g_pendingTmpPool = engine;
        g_pendingTmpPoolAll = g_pendingTmpPoolAll || all;
        g_gcDeferred++;
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*, char)>(kTmpPoolFn)(engine, all);
}

extern "C" void ScenePush(uintptr_t ret)
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_sceneTls));
    if (!s)
    {
        s = static_cast<PreStack*>(calloc(1, sizeof(PreStack)));
        TlsSetValue(g_sceneTls, s);
    }
    // RenderScene starts with UpdateLightSources (0x3010E8BE) and CTerrain::UpdateNodesIncrementaly
    // (0x3010E9E1), which recycle lights and terrain nodes the previous flush still uses: fence here.
    // In fill mode EF_StartEf (slot 112, called at 0x3010E8B5 before both) is the sync point, and the
    // pending releases run at dispatch instead.
    if (s->depth == 0 && GetCurrentThreadId() == g_mainThread && !g_fillOn)
    {
        WaitIdlePublic(297);
        RunPendingGc();
    }
    s->ret[s->depth++] = ret;
}

extern "C" uintptr_t ScenePop()
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_sceneTls));
    const uintptr_t ret = s->ret[--s->depth];
    if (g_sceneSync && GetCurrentThreadId() == g_mainThread)
        WaitIdlePublic(299);
    return ret;
}

// C3DEngine::RenderWorld (0x30110E10): with CRYMT_WORLDSYNC main waits when it returns.
DWORD g_worldTls = TLS_OUT_OF_INDEXES;
bool g_worldSync;

void CensusBegin();

extern "C" void WorldPush(uintptr_t ret)
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_worldTls));
    if (!s)
    {
        s = static_cast<PreStack*>(calloc(1, sizeof(PreStack)));
        TlsSetValue(g_worldTls, s);
    }
    if (s->depth == 0 && GetCurrentThreadId() == g_mainThread)
    {
        CensusBegin();
        if (g_fillOn && g_mode == 2)
        {
            g_fillWindow = true;
            // The other half's proxies belong to the previous frame, whose commands ran before its dispatch.
            g_proxyHalf ^= 1;
            g_proxyUsed = 0;
        }
    }
    s->ret[s->depth++] = ret;
}

extern "C" uintptr_t WorldPop()
{
    auto* s = static_cast<PreStack*>(TlsGetValue(g_worldTls));
    const uintptr_t ret = s->ret[--s->depth];
    if (g_worldSync && GetCurrentThreadId() == g_mainThread)
        WaitIdlePublic(298);
    return ret;
}

extern "C" void WorldThunk();
asm(R"(
.intel_syntax noprefix
.text
.globl WorldThunk
WorldThunk:
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 0x68
    movups [rsp+0x20], xmm0
    movups [rsp+0x30], xmm1
    movups [rsp+0x40], xmm2
    movups [rsp+0x50], xmm3
    mov rcx, [rsp+0x88]
    call WorldPush
    movups xmm0, [rsp+0x20]
    movups xmm1, [rsp+0x30]
    movups xmm2, [rsp+0x40]
    movups xmm3, [rsp+0x50]
    add rsp, 0x68
    pop r9
    pop r8
    pop rdx
    pop rcx
    lea rax, [rip+WorldPost]
    mov [rsp], rax
    movabs rax, 0x30110E10
    jmp rax
WorldPost:
    sub rsp, 0x40
    mov [rsp+0x20], rax
    movups [rsp+0x28], xmm0
    call WorldPop
    mov r11, rax
    mov rax, [rsp+0x20]
    movups xmm0, [rsp+0x28]
    add rsp, 0x40
    jmp r11
.att_syntax prefix
)");

extern "C" void SceneThunk();
asm(R"(
.intel_syntax noprefix
.text
.globl SceneThunk
SceneThunk:
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 0x68
    movups [rsp+0x20], xmm0
    movups [rsp+0x30], xmm1
    movups [rsp+0x40], xmm2
    movups [rsp+0x50], xmm3
    mov rcx, [rsp+0x88]
    call ScenePush
    movups xmm0, [rsp+0x20]
    movups xmm1, [rsp+0x30]
    movups xmm2, [rsp+0x40]
    movups xmm3, [rsp+0x50]
    add rsp, 0x68
    pop r9
    pop r8
    pop rdx
    pop rcx
    lea rax, [rip+ScenePost]
    mov [rsp], rax
    movabs rax, 0x3010E590
    jmp rax
ScenePost:
    sub rsp, 0x40
    mov [rsp+0x20], rax
    movups [rsp+0x28], xmm0
    call ScenePop
    mov r11, rax
    mov rax, [rsp+0x20]
    movups xmm0, [rsp+0x28]
    add rsp, 0x40
    jmp r11
.att_syntax prefix
)");

extern "C" void PreThunk();
asm(R"(
.intel_syntax noprefix
.text
.globl PreThunk
PreThunk:
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 0x28
    mov rcx, [rsp+0x48]
    call PrePush
    add rsp, 0x28
    pop r9
    pop r8
    pop rdx
    pop rcx
    lea rax, [rip+PrePost]
    mov [rsp], rax
    movabs rax, 0x380A6D20
    jmp rax
PrePost:
    sub rsp, 0x40
    mov [rsp+0x20], rax
    movups [rsp+0x28], xmm0
    mov rcx, r13
    call PrePop
    mov r11, rax
    mov rax, [rsp+0x20]
    movups xmm0, [rsp+0x28]
    add rsp, 0x40
    jmp r11
.att_syntax prefix
)");

namespace
{

bool RedirectCall(uintptr_t site, uintptr_t expectedTarget, const void* hook);

// Redirects the rel32 call at kPreprocessCall through a thunk placed within 2 GB of the renderer.
void PatchPreprocessCall()
{
    auto* site = reinterpret_cast<uint8_t*>(kPreprocessCall);
    int32_t rel = 0;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || kPreprocessCall + 5 + rel != kPreprocessFn)
    {
        SetupLog("preprocess call site mismatch, not patched");
        return;
    }
    uint8_t* cave = nullptr;
    for (uintptr_t hint = kRenderBase + kRenderSize + 0x100000; hint < kRenderBase + 0x40000000 && !cave; hint += 0x10000)
        cave = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!cave)
    {
        SetupLog("no code cave near renderer, preprocess not patched");
        return;
    }
    g_preTls = TlsAlloc();
    const auto target = reinterpret_cast<uint64_t>(&PreThunk);
    cave[0] = 0x48; cave[1] = 0xB8; // movabs rax, PreThunk
    memcpy(cave + 2, &target, 8);
    cave[10] = 0xFF; cave[11] = 0xE0; // jmp rax
    DWORD old;
    VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &old);
    const int32_t newRel = static_cast<int32_t>(reinterpret_cast<intptr_t>(cave) - static_cast<intptr_t>(kPreprocessCall + 5));
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(site + 1, &newRel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    SetupLog("preprocess call patched");
    g_shadowTls = TlsAlloc();
    SetupLog(RedirectCall(kShadowPrepCall, kShadowPrepFn, reinterpret_cast<const void*>(&ShadowThunk))
                 ? "shadow prep call patched"
                 : "shadow prep call site mismatch, not patched");
}

// Records every render object main takes, also through nested and renderer-internal calls the gate lets
// pass, so PrepareSkinning sees all characters the flush will skin. EF_GetObject has no direct callers.
using GetObjectFn = void*(__fastcall*)(void*, uint64_t, uint64_t, uint64_t);
void* g_efGetObject;

void* __fastcall RecordGetObject(void* self, uint64_t temp, uint64_t num, uint64_t r9)
{
    void* obj = reinterpret_cast<GetObjectFn>(g_efGetObject)(self, temp, num, r9);
    if (!obj || g_mode != 2)
        return obj;
    if (!(temp & 0xFF) && static_cast<int32_t>(num) < 0)
    {
        const uint16_t id = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(obj) + kObjId);
        if (id < kPermObjectMax)
            g_permObjects[id].store(obj, std::memory_order_relaxed);
    }
    const DWORD tid = GetCurrentThreadId();
    if (tid == g_mainThread)
        g_frameObjects.push_back(reinterpret_cast<uintptr_t>(obj));
    else if (tid == g_rtTid && g_async && g_preDoneSeq.load(std::memory_order_acquire) != g_rtFrameSeq)
        g_rtFrameObjects.push_back(reinterpret_cast<uintptr_t>(obj));
    return obj;
}

void InstallGate(uintptr_t vtableAddr, int count)
{
    auto* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, count * kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto** vtable = reinterpret_cast<void**>(vtableAddr);
    const auto common = reinterpret_cast<uint64_t>(&GateCommon);
    for (int i = 0; i < count; i++)
    {
        uint8_t* s = stubs + i * kStubSize;
        const uint32_t idx = static_cast<uint32_t>(i);
        s[0] = 0xB8; // mov eax, idx
        memcpy(s + 1, &idx, 4);
        s[5] = 0x49; s[6] = 0xBB; // movabs r11, GateCommon
        memcpy(s + 7, &common, 8);
        s[15] = 0x41; s[16] = 0xFF; s[17] = 0xE3; // jmp r11
        g_orig[i] = vtable[i];
    }
    g_efGetObject = g_orig[kEfGetObject];
    g_realGetObject = reinterpret_cast<uintptr_t>(g_efGetObject);
    g_orig[kEfGetObject] = reinterpret_cast<void*>(&RecordGetObject);
    DWORD old;
    VirtualProtect(stubs, count * kStubSize, PAGE_EXECUTE_READ, &old);
    VirtualProtect(vtable, count * sizeof(void*), PAGE_READWRITE, &old);
    for (int i = 0; i < count; i++)
        vtable[i] = stubs + i * kStubSize;
    vtable[kEndEf3DSlot] = reinterpret_cast<void*>(&HookEndEf3D);
    VirtualProtect(vtable, count * sizeof(void*), old, &old);
    g_slotCount = count;
}

void InstallAuxGate()
{
    auto** vtable = reinterpret_cast<void**>(kAuxVtable);
    if (reinterpret_cast<uintptr_t>(vtable[0]) != kAuxFirstFn)
    {
        SetupLog("aux vtable mismatch, aux gate not installed");
        return;
    }
    auto* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, kAuxSlots * kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    const auto common = reinterpret_cast<uint64_t>(&GateCommon);
    for (int i = 0; i < kAuxSlots; i++)
    {
        uint8_t* s = stubs + i * kStubSize;
        const uint32_t idx = static_cast<uint32_t>(kAuxBase + i);
        s[0] = 0xB8;
        memcpy(s + 1, &idx, 4);
        s[5] = 0x49; s[6] = 0xBB;
        memcpy(s + 7, &common, 8);
        s[15] = 0x41; s[16] = 0xFF; s[17] = 0xE3;
        g_orig[kAuxBase + i] = vtable[i];
    }
    DWORD old;
    VirtualProtect(stubs, kAuxSlots * kStubSize, PAGE_EXECUTE_READ, &old);
    VirtualProtect(vtable, kAuxSlots * sizeof(void*), PAGE_READWRITE, &old);
    for (int i = 0; i < kAuxSlots; i++)
        vtable[i] = stubs + i * kStubSize;
    VirtualProtect(vtable, kAuxSlots * sizeof(void*), old, &old);
    SetupLog("aux gate installed");
}

const char* RttiName(uintptr_t base, uintptr_t vtableAddr)
{
    const auto col = *reinterpret_cast<uintptr_t*>(vtableAddr - 8);
    if (col < base || col >= base + kRenderSize)
        return "?";
    const uint32_t typeRva = *reinterpret_cast<uint32_t*>(col + 12);
    return reinterpret_cast<const char*>(base + typeRva + 16);
}

void PatchDeviceVtable(void* device)
{
    auto** vtable = *reinterpret_cast<void***>(device);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(vtable[0], &mbi, sizeof(mbi));
    const auto modBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(modBase + reinterpret_cast<IMAGE_DOS_HEADER*>(modBase)->e_lfanew);
    const uintptr_t modEnd = modBase + nt->OptionalHeader.SizeOfImage;
    int count = 0;
    while (count < kMaxDevSlots)
    {
        const auto v = reinterpret_cast<uintptr_t>(vtable[count]);
        if (v < modBase || v >= modEnd)
            break;
        count++;
    }
    auto* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, count * kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    const auto common = reinterpret_cast<uint64_t>(&DevGateCommon);
    for (int i = 0; i < count; i++)
    {
        uint8_t* s = stubs + i * kStubSize;
        const uint32_t idx = static_cast<uint32_t>(i);
        s[0] = 0xB8;
        memcpy(s + 1, &idx, 4);
        s[5] = 0x49; s[6] = 0xBB;
        memcpy(s + 7, &common, 8);
        s[15] = 0x41; s[16] = 0xFF; s[17] = 0xE3;
        g_devOrig[i] = vtable[i];
    }
    DWORD old;
    VirtualProtect(stubs, count * kStubSize, PAGE_EXECUTE_READ, &old);
    VirtualProtect(vtable, count * sizeof(void*), PAGE_READWRITE, &old);
    for (int i = 3; i < count; i++)
        vtable[i] = stubs + i * kStubSize;
    VirtualProtect(vtable, count * sizeof(void*), old, &old);
    g_devSlotCount = count;
    char line[128];
    sprintf_s(line, "device vtable %p slots=%d", static_cast<void*>(vtable), count);
    SetupLog(line);
}

HRESULT WINAPI HookCreateDevice(void* adapter, int type, HMODULE sw, UINT flags, UINT sdk, void** device)
{
    char line[160];
    const UINT newFlags = g_mode == 2 ? (flags & ~1u) : flags;
    const HRESULT hr = g_realCreateDevice(adapter, type, sw, newFlags, sdk, device);
    sprintf_s(line, "D3D10CreateDevice flags=0x%x -> 0x%x hr=0x%lx", flags, newFlags, static_cast<unsigned long>(hr));
    SetupLog(line);
    if (SUCCEEDED(hr) && device && *device)
    {
        static const GUID kMultithread = {0x9B7E4E00, 0x342C, 0x4106, {0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0}};
        auto* unk = static_cast<IUnknown*>(*device);
        void* mt = nullptr;
        if (SUCCEEDED(unk->QueryInterface(kMultithread, &mt)) && mt)
        {
            using SetFn = BOOL(STDMETHODCALLTYPE*)(void*, BOOL);
            using GetFn = BOOL(STDMETHODCALLTYPE*)(void*);
            auto** vt = *reinterpret_cast<void***>(mt);
            const BOOL was = reinterpret_cast<GetFn>(vt[6])(mt);
            if (g_mode == 2)
                reinterpret_cast<SetFn>(vt[5])(mt, TRUE);
            sprintf_s(line, "multithread protected was=%d now=%d", was, reinterpret_cast<GetFn>(vt[6])(mt));
            SetupLog(line);
            static_cast<IUnknown*>(mt)->Release();
        }
        if (g_mode == 2)
            PatchDeviceVtable(*device);
    }
    WriteStats();
    return hr;
}

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC p = g_realGetProcAddress(module, name);
    if (p && HIWORD(reinterpret_cast<uintptr_t>(name)) && strcmp(name, "D3D10CreateDevice") == 0)
    {
        g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(p);
        return reinterpret_cast<FARPROC>(&HookCreateDevice);
    }
    return p;
}

void PatchGetProcAddress(uintptr_t base)
{
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; imp++)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "kernel32.dll") != 0)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), "GetProcAddress") != 0)
                continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            g_realGetProcAddress = reinterpret_cast<GetProcFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
            iat->u1.Function = reinterpret_cast<ULONGLONG>(&HookGetProcAddress);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            SetupLog("GetProcAddress import patched");
            return;
        }
    }
    SetupLog("GetProcAddress import not found");
}

void TryPatchRenderer(uintptr_t base)
{
    if (base != kRenderBase)
    {
        SetupLog("renderer not at preferred base, hooks disabled");
        return;
    }
    PatchGetProcAddress(base);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t textBegin = 0, textEnd = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
        {
            textBegin = base + sec[i].VirtualAddress;
            textEnd = textBegin + sec[i].Misc.VirtualSize;
            break;
        }

    uintptr_t best = 0;
    int bestCount = 0;
    char line[256];
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (sec[i].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))
            continue;
        const uintptr_t from = base + sec[i].VirtualAddress;
        const uintptr_t to = from + (sec[i].Misc.VirtualSize & ~size_t{7});
        for (uintptr_t p = from; p < to; p += 8)
        {
            if (*reinterpret_cast<uintptr_t*>(p) != kEndEf3D)
                continue;
            const uintptr_t vt = p - kEndEf3DSlot * 8;
            if (vt < from)
                continue;
            int count = 0;
            while (count < kMaxSlots)
            {
                const auto v = *reinterpret_cast<uintptr_t*>(vt + count * 8);
                if (v < textBegin || v >= textEnd)
                    break;
                count++;
            }
            sprintf_s(line, "vtable candidate %llx slots=%d class=%s", static_cast<unsigned long long>(vt), count, RttiName(base, vt));
            SetupLog(line);
            if (count > bestCount)
            {
                best = vt;
                bestCount = count;
            }
        }
    }
    if (!best || bestCount <= kSetState)
    {
        SetupLog("IRenderer vtable not found, hooks disabled");
        return;
    }
    InstallGate(best, bestCount);
    SetupLog("gate installed");
    if (g_mode == 2)
        InstallAuxGate();
    if (g_mode == 2)
        PatchPreprocessCall();
}

LONG g_faults;

void PrintUnwound(FILE* f, CONTEXT ctx)
{
    for (int i = 0; i < 28 && ctx.Rip; i++)
    {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(ctx.Rip), &mod);
        if (mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        fprintf(f, "  %s+0x%llx\n", base ? base + 1 : name,
                static_cast<unsigned long long>(ctx.Rip - reinterpret_cast<uintptr_t>(mod)));
        DWORD64 imageBase = 0;
        RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn)
        {
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
}

// Logs the thread and unwound stack of the first faults, then lets the normal handlers run.
LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // c1-launcher raises 0xE0C1C100..103 for abort, pure call, invalid parameter and engine errors (CryFatalError, the
    // message in ExceptionInformation[0]); the engine often ends before Game.log has it, so it is kept here.
    const bool launcher = (code & 0xFFFFFFFC) == 0xE0C1C100;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_ILLEGAL_INSTRUCTION && !launcher)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_faults) > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    FILE* f = nullptr;
    if (fopen_s(&f, g_faultPath.c_str(), "a") != 0 || !f)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD tid = GetCurrentThreadId();
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u build %s %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            __DATE__, __TIME__);
    fprintf(f, "fault 0x%lx at %p thread=%lu (%s) frame=%llu read=%llu write=%llu async=%d\n", code,
            info->ExceptionRecord->ExceptionAddress, tid,
            tid == g_mainThread ? "main" : tid == g_rtTid ? "render" : "other", static_cast<unsigned long long>(g_frame),
            static_cast<unsigned long long>(g_read.load()), static_cast<unsigned long long>(g_write.load()), g_async ? 1 : 0);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        fprintf(f, "  %s address %llx\n", info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
    if (code == 0xE0C1C103 && info->ExceptionRecord->NumberParameters >= 1 && info->ExceptionRecord->ExceptionInformation[0])
        fprintf(f, "  engine error: %.400s\n", reinterpret_cast<const char*>(info->ExceptionRecord->ExceptionInformation[0]));
    const CONTEXT& c = *info->ContextRecord;
    fprintf(f, "  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx\n  r8=%llx r9=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
            c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.R12, c.R13, c.R14, c.R15);
    PrintUnwound(f, *info->ContextRecord);
    if (tid == g_rtTid && g_mainThread)
    {
        HANDLE main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainThread);
        if (main && SuspendThread(main) != static_cast<DWORD>(-1))
        {
            CONTEXT mc{};
            mc.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(main, &mc))
            {
                fprintf(f, "main thread at the same moment:\n");
                PrintUnwound(f, mc);
            }
            ResumeThread(main);
        }
        if (main)
            CloseHandle(main);
    }
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Watchdog: when the frame counter stays still for 10 s after the game has rendered, the stacks of main and
// the render thread go to logs\mt\crymt_hang.txt, once per stall.
void DumpThreadStack(FILE* f, DWORD tid, const char* name)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
        return;
    if (SuspendThread(h) != static_cast<DWORD>(-1))
    {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c))
        {
            fprintf(f, "%s thread %lu:\n", name, tid);
            PrintUnwound(f, c);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

DWORD WINAPI WatchdogMain(void*)
{
    uint64_t last = 0;
    int still = 0;
    bool dumped = false;
    for (;;)
    {
        Sleep(1000);
        const uint64_t now = g_frame;
        still = now == last && now > 100 ? still + 1 : 0;
        dumped = dumped && still > 0;
        last = now;
        if (still < 10 || dumped)
            continue;
        dumped = true;
        FILE* f = nullptr;
        if (fopen_s(&f, g_hangPath.c_str(), "a") != 0 || !f)
            continue;
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02u:%02u:%02u stall at frame %llu read=%llu write=%llu async=%d fill window=%d build %s %s\n", st.wHour, st.wMinute,
                st.wSecond, static_cast<unsigned long long>(now), static_cast<unsigned long long>(g_read.load()),
                static_cast<unsigned long long>(g_write.load()), g_async ? 1 : 0, g_fillWindow ? 1 : 0, __DATE__, __TIME__);
        DumpThreadStack(f, g_mainThread, "main");
        DumpThreadStack(f, g_rtTid, "render");
        fclose(f);
    }
}

// Points a rel32 call (or tail jump) at a 12-byte absolute jump placed within 2 GB of the site.
bool RedirectCall(uintptr_t site, uintptr_t expectedTarget, const void* hook)
{
    auto* p = reinterpret_cast<uint8_t*>(site);
    int32_t rel = 0;
    memcpy(&rel, p + 1, 4);
    if ((p[0] != 0xE8 && p[0] != 0xE9) || site + 5 + rel != expectedTarget)
        return false;
    uint8_t* cave = nullptr;
    for (uintptr_t hint = (site & ~uintptr_t{0xFFFF}) + 0x4000000; hint < site + 0x40000000 && !cave; hint += 0x10000)
        cave = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!cave)
        return false;
    const auto target = reinterpret_cast<uint64_t>(hook);
    cave[0] = 0x48; cave[1] = 0xB8;
    memcpy(cave + 2, &target, 8);
    cave[10] = 0xFF; cave[11] = 0xE0;
    DWORD old;
    VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &old);
    const int32_t newRel = static_cast<int32_t>(reinterpret_cast<intptr_t>(cave) - static_cast<intptr_t>(site + 5));
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &newRel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

void PatchRenderSceneVtable(uintptr_t base)
{
    const bool gc1 = RedirectCall(kTerrainUnloadCall, kTerrainUnloadFn, reinterpret_cast<const void*>(&DeferTerrainUnload));
    const bool gc2 = RedirectCall(kTmpPoolCall, kTmpPoolFn, reinterpret_cast<const void*>(&DeferTmpPool));
    char gcLine[64];
    sprintf_s(gcLine, "gc calls redirected: terrain=%d tmppool=%d", gc1, gc2);
    SetupLog(gcLine);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    g_sceneTls = TlsAlloc();
    g_worldTls = TlsAlloc();
    int patched = 0;
    int worldPatched = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (sec[i].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))
            continue;
        auto* from = reinterpret_cast<uintptr_t*>(base + sec[i].VirtualAddress);
        const size_t n = sec[i].Misc.VirtualSize / 8;
        for (size_t k = 0; k < n; k++)
        {
            uintptr_t replacement = 0;
            if (from[k] == kRenderSceneFn)
            {
                replacement = reinterpret_cast<uintptr_t>(&SceneThunk);
                patched++;
            }
            else if (from[k] == kRenderWorldFn)
            {
                replacement = reinterpret_cast<uintptr_t>(&WorldThunk);
                worldPatched++;
            }
            if (!replacement)
                continue;
            DWORD old;
            VirtualProtect(&from[k], 8, PAGE_READWRITE, &old);
            from[k] = replacement;
            VirtualProtect(&from[k], 8, old, &old);
        }
    }
    char line[96];
    sprintf_s(line, "RenderScene vtable entries patched=%d RenderWorld=%d", patched, worldPatched);
    SetupLog(line);
}

struct LdrNotification
{
    ULONG flags;
    const UNICODE_STRING* fullName;
    const UNICODE_STRING* baseName;
    void* base;
    ULONG size;
};

} // namespace

// Replaces whole, position-independent first instructions of fn (expect, 5..16 bytes) with a jump to hook through a
// cave near the module; returns a trampoline that runs them and continues in fn.
void* DetourHead(uintptr_t fn, const uint8_t* expect, size_t len, const void* hook)
{
    if (len < 5 || len > 16 || memcmp(reinterpret_cast<const void*>(fn), expect, len) != 0)
        return nullptr;
    uint8_t* cave = nullptr;
    for (uintptr_t hint = (fn & ~uintptr_t{0xFFFF}) + 0x4000000; hint < fn + 0x40000000 && !cave; hint += 0x10000)
        cave = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!cave)
        return nullptr;
    const auto target = reinterpret_cast<uint64_t>(hook);
    cave[0] = 0x48; cave[1] = 0xB8; // movabs rax, hook; jmp rax
    memcpy(cave + 2, &target, 8);
    cave[10] = 0xFF; cave[11] = 0xE0;
    // The moved instructions; jmp [rip+0] to fn + len, which keeps every register (heads like mov rax, rsp set rax).
    uint8_t* tramp = cave + 16;
    memcpy(tramp, expect, len);
    const uint64_t back = fn + len;
    const uint8_t jmpRip[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(tramp + len, jmpRip, 6);
    memcpy(tramp + len + 6, &back, 8);
    DWORD old;
    VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &old);
    auto* p = reinterpret_cast<uint8_t*>(fn);
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(cave) - static_cast<intptr_t>(fn + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < len; i++)
        p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return tramp;
}

// The global mesh list: main links new meshes (CRYMT_TERRAINOWN) while the render thread runs queued deletions.
CRITICAL_SECTION g_meshListCs;
using MeshCtorFn = void*(__fastcall*)(void*, void*, void*);
using MeshDtorFn = void(__fastcall*)(void*);
MeshCtorFn g_meshCtor;
MeshDtorFn g_meshDtor;

void* __fastcall MeshCtorHook(void* self, void* type, void* source)
{
    EnterCriticalSection(&g_meshListCs);
    void* r = g_meshCtor(self, type, source);
    LeaveCriticalSection(&g_meshListCs);
    return r;
}

void __fastcall MeshDtorHook(void* self)
{
    EnterCriticalSection(&g_meshListCs);
    g_meshDtor(self);
    LeaveCriticalSection(&g_meshListCs);
}

void InstallMeshListHooks()
{
    if (!GetEnvironmentVariableW(L"CRYMT_TERRAINOWN", nullptr, 0))
        return;
    InitializeCriticalSection(&g_meshListCs);
    g_meshCtor = reinterpret_cast<MeshCtorFn>(DetourHead(kMeshCtorFn, kMeshCtorHead, sizeof(kMeshCtorHead), reinterpret_cast<const void*>(&MeshCtorHook)));
    g_meshDtor = reinterpret_cast<MeshDtorFn>(DetourHead(kMeshDtorFn, kMeshDtorHead, sizeof(kMeshDtorHead), reinterpret_cast<const void*>(&MeshDtorHook)));
    g_meshListHooked = g_meshCtor && g_meshDtor;
    g_terrainOwn = g_meshListHooked;
    SetupLog(g_terrainOwn ? "terrain ownership: mesh list guarded" : "terrain ownership: mesh constructor/destructor not hooked");
}

// PrepareDepthMap's caster call (render thread): replay main's recording of this caster for this frustum, else render.
extern "C" void __fastcall CasterRenderHook(void* caster, uint8_t* rp, uint8_t* frustum)
{
    if (g_shadowRec && !g_recIndex.empty() && GetCurrentThreadId() == g_rtTid)
    {
        static uint8_t* lastCopy;
        static uint64_t lastSeq;
        static uintptr_t lastOrig;
        if (frustum != lastCopy || lastSeq != g_rtFrameSeq)
        {
            lastCopy = frustum;
            lastSeq = g_rtFrameSeq;
            lastOrig = reinterpret_cast<uintptr_t>(frustum);
            for (const FlushCopy& c : g_flushCopies)
                if (c.copy == frustum)
                {
                    lastOrig = reinterpret_cast<uintptr_t>(c.orig);
                    break;
                }
        }
        const auto it = g_recIndex.find(RecKey(reinterpret_cast<uintptr_t>(caster), lastOrig));
        if (it != g_recIndex.end())
        {
            CasterRecording& r = g_recordings[it->second];
            if (r.caster == reinterpret_cast<uintptr_t>(caster) && r.frustum == lastOrig)
            {
                float distance, alpha;
                memcpy(&distance, rp + 0x50, 4);
                memcpy(&alpha, rp + 0x38, 4);
                if (fabsf(distance - r.distance) > 0.01f + 0.001f * r.distance || alpha != 1.0f)
                    g_recParamMismatch++;
                for (uint32_t k = 0; k < r.count; k++)
                    ExecuteCommand(g_recCommands[r.first + k]);
                g_recReplays++;
                return;
            }
        }
        g_recFallbacks++;
    }
    using RenderFn = void(__fastcall*)(void*, void*);
    reinterpret_cast<RenderFn>((*static_cast<void***>(caster))[1])(caster, rp);
}

// Replaces the 17 bytes at kCasterRenderCall with "call cave" + nops; the cave passes caster (rbx), the SRendParams
// (rsp+0x2C0 of PrepareDepthMap, +8 for the return address) and the frustum (rdi) to CasterRenderHook.
bool PatchCasterCall()
{
    auto* site = reinterpret_cast<uint8_t*>(kCasterRenderCall);
    if (memcmp(site, kCasterRenderCode, sizeof(kCasterRenderCode)) != 0)
        return false;
    uint8_t* cave = nullptr;
    for (uintptr_t hint = kRenderBase + kRenderSize + 0x200000; hint < kRenderBase + 0x40000000 && !cave; hint += 0x10000)
        cave = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!cave)
        return false;
    const uint8_t thunk[] = {0x48, 0x8D, 0x94, 0x24, 0xC8, 0x02, 0x00, 0x00, // lea rdx, [rsp+0x2C8]
                             0x48, 0x8B, 0xCB,                               // mov rcx, rbx
                             0x4C, 0x8B, 0xC7,                               // mov r8, rdi
                             0x48, 0xB8};                                    // movabs rax, CasterRenderHook
    memcpy(cave, thunk, sizeof(thunk));
    const auto hook = reinterpret_cast<uint64_t>(&CasterRenderHook);
    memcpy(cave + sizeof(thunk), &hook, 8);
    cave[sizeof(thunk) + 8] = 0xFF; // jmp rax
    cave[sizeof(thunk) + 9] = 0xE0;
    DWORD old;
    VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &old);
    VirtualProtect(site, sizeof(kCasterRenderCode), PAGE_EXECUTE_READWRITE, &old);
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(cave) - reinterpret_cast<intptr_t>(site + 5));
    site[0] = 0xE8;
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, sizeof(kCasterRenderCode) - 5);
    VirtualProtect(site, sizeof(kCasterRenderCode), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kCasterRenderCode));
    return true;
}

void InstallShadowRecording()
{
    if (!GetEnvironmentVariableW(L"CRYMT_SHADOWREC", nullptr, 0))
        return;
    g_shadowRec = PatchCasterCall();
    SetupLog(g_shadowRec ? "shadow casters recorded on main" : "shadow caster call differs, recording off");
}

// CTerrainNode::RenderSector's in-place update (0x300C3139): build the vertices as for a LOD change and recreate the
// mesh, so main never edits a mesh the render thread may be drawing.
using TerrainUpdateFn = void(__fastcall*)(uintptr_t, uintptr_t, uint64_t);
using TerrainBuildFn = void(__fastcall*)(uintptr_t, int, int);

extern "C" void __fastcall TerrainReuseHook(uintptr_t node, uintptr_t buffer, uint64_t force)
{
    if (g_terrainOwn && g_fillWindow && g_async && GetCurrentThreadId() == g_mainThread)
    {
        const int lodLimit = *reinterpret_cast<const int*>(kTerrainStepBase) << (*reinterpret_cast<const int*>(node + kTerrainNodeLodShift) & 31);
        const int sector = (1 << (*reinterpret_cast<const uint8_t*>(node + kTerrainNodeLod) & 31)) * *reinterpret_cast<const int*>(kTerrainUnitSize);
        reinterpret_cast<TerrainBuildFn>(kTerrainBuildVerticesFn)(node, std::min(lodLimit, sector), 0);
        reinterpret_cast<TerrainUpdateFn>(kTerrainUpdateRenderMeshFn)(node, buffer, 1);
        g_terrainRebuilds++;
        return;
    }
    reinterpret_cast<TerrainUpdateFn>(kTerrainUpdateRenderMeshFn)(node, buffer, force);
}

// The sector texture ids (FUN_300BF050, see Crysis6156.h) go into the mesh's render element directly. In the fill
// window that write landed while the render thread still drew the previous frame: the sector got the texture slot the
// next frame assigned while that texture's new contents were still queued (texture updates run in order, after that
// flush), so it showed another sector's pooled texture for one frame (terrain under the trees flickering on the gitest
// demo end, every ~23 frames). When the ids change, the whole call is queued and runs after the flush; a mesh main
// made in this window is not drawn yet. Off by default (CRYMT_TERRAINTEX=1): it did not change the measured flicker,
// which was the coverage buffer (NOTES).
constexpr uint32_t kTerrainTexturesReason = 605;

bool TerrainTexturesChange(uintptr_t node)
{
    const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(node + 0xC0);
    const uintptr_t mesh = owner ? *reinterpret_cast<const uintptr_t*>(owner + 0x40) : 0;
    if (!mesh || IsMainMesh(mesh))
        return false;
    const auto* chunks = *reinterpret_cast<const uint8_t* const*>(mesh + kMeshChunks);
    if (!chunks || !*reinterpret_cast<const int*>(chunks + 8))
        return false;
    const uintptr_t re = *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(chunks) + 0x20);
    if (!re)
        return false;
    const int debug = *reinterpret_cast<const int*>(*reinterpret_cast<const uintptr_t*>(k3DCVarsGlobal) + 0xF4);
    const uint32_t texture = debug == 0   ? *reinterpret_cast<const uint16_t*>(node + kTerrainNodeTexture)
                             : debug == 1 ? *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uintptr_t*>(kTerrainGlobal) + 0x24)
                                          : 0;
    return *reinterpret_cast<const uint32_t*>(re + 0x18) != texture ||
           *reinterpret_cast<const uint32_t*>(re + 0x1C) != *reinterpret_cast<const uint16_t*>(node + kTerrainNodeTexture2);
}

extern "C" void __fastcall TerrainTexturesHook(uintptr_t node)
{
    if (g_terrainTexQueue && g_fillWindow && g_async && GetCurrentThreadId() == g_mainThread && TerrainTexturesChange(node))
    {
        Command& c = BeginCommand(0);
        c.slot = kTerrainTexturesReason;
        c.target = reinterpret_cast<void*>(kTerrainTexturesFn);
        c.regs = {};
        c.regs.rcx = node;
        memset(c.stack, 0, sizeof(c.stack));
        PublishCommand();
        g_terrainTexQueued++;
        return;
    }
    reinterpret_cast<void(__fastcall*)(uintptr_t)>(kTerrainTexturesFn)(node);
}

// Voxel objects and roads are rebuilt in C3DEngine::Update (see Crysis6156.h) while the render thread may still draw
// the previous frame with their old mesh data (the far relief under the trees flickered every ~24 frames on the gitest
// demo end as the camera moved). When one is queued, main waits for the render thread first (reasons 434/435); most
// frames have none. Off by default (CRYMT_COMPILEFENCE=1): the queues were empty at the measured flicker.
using CompileFn = bool(__fastcall*)(uintptr_t);

bool CompileQueued(uintptr_t countOffset)
{
    const uintptr_t engine = *reinterpret_cast<const uintptr_t*>(k3DEngineGlobal);
    return engine && *reinterpret_cast<const int*>(engine + countOffset) > 0;
}

extern "C" bool __fastcall VoxelCompileHook(uintptr_t terrain)
{
    if (g_mode == 2 && g_async && GetCurrentThreadId() == g_mainThread && CompileQueued(kVoxelQueueCount))
        WaitIdle(434);
    return reinterpret_cast<CompileFn>(kVoxelCompileFn)(terrain);
}

extern "C" bool __fastcall RoadCompileHook(uintptr_t terrain)
{
    if (g_mode == 2 && g_async && GetCurrentThreadId() == g_mainThread && CompileQueued(kRoadQueueCount))
        WaitIdle(435);
    return reinterpret_cast<CompileFn>(kRoadCompileFn)(terrain);
}

// Material reference counts: the render thread releases materials (queued mesh deletions, the destructor's
// material Release; queued SetMaterial) while main adds and drops references to the same ones, and CMatInfo
// changes its count without a lock. Both functions are rewritten in place with locked instructions.
// The destructor unregisters the material from the material manager's tables, which main reads and fills (lookups,
// level loading) without a lock: a material deleted on the render thread corrupted them (a deleted material left in the
// list the unload leak check walks, 0x300E8F9C on the third level change). The last release on another thread than
// main queues the material; main deletes it at the next frame start or level change, unless it was taken again
// (a lookup on main can AddRef a material at zero, as in the original).
std::mutex g_matLock;
std::vector<void*> g_matPending;
uint64_t g_matDeferred, g_matRevived;

void DestroyMaterial(void* mat)
{
    reinterpret_cast<void(__fastcall*)(void*, int)>((*static_cast<void* const* const*>(mat))[0])(mat, 1); // scalar deleting destructor
}

void __fastcall MaterialRelease(void* mat)
{
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(mat) + 0x24)) > 0)
        return;
    if (g_mode == 2 && g_mainThread && GetCurrentThreadId() != g_mainThread)
    {
        std::lock_guard<std::mutex> guard(g_matLock);
        g_matPending.push_back(mat);
        g_matDeferred++;
        return;
    }
    {
        std::lock_guard<std::mutex> guard(g_matLock);
        g_matPending.erase(std::remove(g_matPending.begin(), g_matPending.end(), mat), g_matPending.end());
    }
    DestroyMaterial(mat);
}

void RunPendingMaterialDeletes()
{
    std::vector<void*> list;
    {
        std::lock_guard<std::mutex> guard(g_matLock);
        list.swap(g_matPending);
    }
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    for (void* mat : list)
        if (*reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(mat) + 0x24) <= 0)
            DestroyMaterial(mat);
        else
            g_matRevived++;
}

bool PatchMaterialRefCount()
{
    static const uint8_t kAddRef[] = {0xF0, 0x83, 0x41, 0x24, 0x01, 0xC3}; // lock add dword [rcx+0x24], 1; ret
    uint8_t release[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0}; // mov rax, MaterialRelease; jmp rax
    const uint64_t target = reinterpret_cast<uint64_t>(&MaterialRelease);
    memcpy(release + 2, &target, 8);
    static_assert(sizeof(release) <= sizeof(kMatReleaseCode), "fits the original");
    auto* add = reinterpret_cast<uint8_t*>(kMatAddRefFn);
    auto* rel = reinterpret_cast<uint8_t*>(kMatReleaseFn);
    if (memcmp(add, kMatAddRefCode, sizeof(kMatAddRefCode)) != 0 || memcmp(rel, kMatReleaseCode, sizeof(kMatReleaseCode)) != 0)
        return false;
    DWORD old;
    VirtualProtect(add, sizeof(kAddRef), PAGE_EXECUTE_READWRITE, &old);
    memcpy(add, kAddRef, sizeof(kAddRef));
    VirtualProtect(add, sizeof(kAddRef), old, &old);
    VirtualProtect(rel, sizeof(release), PAGE_EXECUTE_READWRITE, &old);
    memcpy(rel, release, sizeof(release));
    VirtualProtect(rel, sizeof(release), old, &old);
    FlushInstructionCache(GetCurrentProcess(), add, 0x40);
    return true;
}

// Terrain surface (detail layer) meshes: an existing one the render thread may draw is released through the queue
// (DeleteRenderMesh = Release, after the sector mesh's, whose destructor clears its dependents' container pointer)
// and the update creates a new one main owns.
using SurfaceUpdateFn = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t*, int, uintptr_t, uintptr_t);

extern "C" void __fastcall SurfaceUpdateHook(uintptr_t mesh, uintptr_t info, uintptr_t* slot, int axis, uintptr_t inds, uintptr_t name)
{
    if (g_terrainOwn && g_fillWindow && g_async && GetCurrentThreadId() == g_mainThread && slot && *slot && !IsMainMesh(*slot))
    {
        auto* renderer = reinterpret_cast<void*>(g_renderer);
        using DeleteMeshFn = void(__fastcall*)(void*, uintptr_t);
        reinterpret_cast<DeleteMeshFn>((*static_cast<void***>(renderer))[kDeleteRenderMesh])(renderer, *slot); // queued
        *slot = 0;
        g_surfacesRecreated++;
    }
    reinterpret_cast<SurfaceUpdateFn>(kTerrainSurfaceUpdateFn)(mesh, info, slot, axis, inds, name);
}

// Deferred reclamation: while a flush is in flight, memory freed by the main thread may still be read
// by the render thread, so CryFree/CryRealloc/CrySystemCrtFree from main are held until it is idle.
// c1-launcher exports these from Crysis.exe and the engine modules resolve them there.
using CryMallocFn = void*(__cdecl*)(size_t, size_t&);
using CryReallocFn = void*(__cdecl*)(void*, size_t, size_t&);
using CryFreeFn = size_t(__cdecl*)(void*);
using CryGetMemSizeFn = size_t(__cdecl*)(void*, size_t);
using CrtFreeFn = void(__cdecl*)(void*);
CryMallocFn g_origCryMalloc;
CryReallocFn g_origCryRealloc;
CryFreeFn g_origCryFree;
CryGetMemSizeFn g_origCryGetMemSize;
CrtFreeFn g_origCrtFree;
std::vector<void*> g_pendingFrees;
std::vector<void*> g_pendingCrtFrees;
uint64_t g_freesDeferred;

bool DeferFreesNow()
{
    return g_mode == 2 && g_async && g_frame >= g_warmupFrames && GetCurrentThreadId() == g_mainThread;
}

void RunDeferredFrees()
{
    for (void* p : g_pendingFrees)
        g_origCryFree(p);
    g_pendingFrees.clear();
    for (void* p : g_pendingCrtFrees)
        g_origCrtFree(p);
    g_pendingCrtFrees.clear();
}

extern "C" size_t __cdecl HookCryFree(void* p)
{
    if (p && DeferFreesNow())
    {
        g_pendingFrees.push_back(p);
        g_freesDeferred++;
        return g_origCryGetMemSize(p, 0);
    }
    return g_origCryFree(p);
}

extern "C" void* __cdecl HookCryRealloc(void* p, size_t size, size_t& allocated)
{
    if (p && DeferFreesNow())
    {
        const size_t oldSize = g_origCryGetMemSize(p, 0);
        void* n = g_origCryMalloc(size, allocated);
        if (n)
            memcpy(n, p, std::min(oldSize, size));
        g_pendingFrees.push_back(p);
        g_freesDeferred++;
        return n;
    }
    return g_origCryRealloc(p, size, allocated);
}

extern "C" void __cdecl HookCrtFree(void* p)
{
    if (p && DeferFreesNow())
    {
        g_pendingCrtFrees.push_back(p);
        g_freesDeferred++;
        return;
    }
    g_origCrtFree(p);
}

namespace
{
// Points an export of a module at a jump thunk placed within 4 GB above it; returns the old target.
void* PatchModuleExport(uintptr_t base, const char* name, const void* hook)
{
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress)
        return nullptr;
    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    auto* ordinals = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; i++)
    {
        if (strcmp(reinterpret_cast<const char*>(base + names[i]), name) != 0)
            continue;
        DWORD* entry = &funcs[ordinals[i]];
        void* old = reinterpret_cast<void*>(base + *entry);
        uint8_t* cave = nullptr;
        for (uintptr_t hint = base + 0x10000000; hint < base + 0xF0000000 && !cave; hint += 0x10000)
            cave = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!cave)
            return nullptr;
        const auto target = reinterpret_cast<uint64_t>(hook);
        cave[0] = 0x48; cave[1] = 0xB8;
        memcpy(cave + 2, &target, 8);
        cave[10] = 0xFF; cave[11] = 0xE0;
        DWORD prot;
        VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &prot);
        VirtualProtect(entry, 4, PAGE_READWRITE, &prot);
        *entry = static_cast<DWORD>(reinterpret_cast<uintptr_t>(cave) - base);
        VirtualProtect(entry, 4, prot, &prot);
        return old;
    }
    return nullptr;
}

void* PatchExeExport(const char* name, const void* hook)
{
    return PatchModuleExport(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), name, hook);
}
} // namespace

// Exports for D3DLayer.cpp.
void* CryMTPatchModuleExport(uintptr_t base, const char* name, const void* hook)
{
    return PatchModuleExport(base, name, hook);
}

void CryMTSetupLog(const char* text)
{
    SetupLog(text);
}

int CryMTPatchImports(uintptr_t base, const char* dll, const char* const* names, const void* const* hooks, int count);

namespace
{

// Pause menu when the game loses focus (CRYMT_PAUSEONFOCUS). The framework's PreUpdate (IGameFramework slot 9, Mod SDK
// order) runs on main every frame with the focus flag; on the frame focus goes, an Escape key event is posted through
// IInput::PostInputEvent (slot 7), which reaches the HUD exactly like the key: it opens the in-game menu and pauses
// the game, with its own checks for cutscenes and modal screens. Only while a game runs and is not paused already
// (IsGameStarted slot 15, IsGamePaused slot 14), so an open menu is never closed. The framework and the input system
// come from the CreateGameFramework / CreateInput exports.
struct InputEvent // SInputEvent, Mod SDK IInput.h
{
    int deviceId, state;
    const char* keyName;
    int keyId;
    uint32_t timestamp;
    int modifiers;
    float value;
    void* symbol;
};
void PatchVtableSlot(uintptr_t vtable, int slot, uintptr_t* original, const void* hook);
using CreateExportFn = void*(*)(void*, void*, void*, void*); // arguments passed through as they came
using PreUpdateFn = bool (*)(void*, bool, unsigned int);
using FrameworkQueryFn = bool (*)(void*);
using PostInputEventFn = void (*)(void*, const InputEvent&);
CreateExportFn g_origCreateFramework, g_origCreateInput;
PreUpdateFn g_origPreUpdate;
void* g_input;
bool g_pauseOnFocus, g_hadFocus = true;
uint64_t g_focusPauses;

void FocusLog(const char* format, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_focus.txt").c_str(), "a") != 0 || !f)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// CGame::Update always passes haveFocus = true (Mod SDK Game.cpp), so focus is whether the foreground window is ours.
bool GameHasFocus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Test tool (CRYMT_TESTCMD="map village;map island", CRYMT_TESTCMDSEC=60): console commands on main, each that many
// seconds after the previous one, e.g. level changes. IConsole::ExecuteString is slot 31 (Mod SDK order); the console is
// gEnv->pConsole (+0x30) of CryAction's gEnv (0x30964AE0, read by IsGamePaused).
constexpr uintptr_t kCryActionEnv = 0x30964AE0;
char g_testCommands[512];
size_t g_testNext;
int g_testInterval;
DWORD g_testLast;

void ConsoleCommand(const char* command)
{
    const auto* env = *reinterpret_cast<uint8_t* const*>(kCryActionEnv);
    void* console = env ? *reinterpret_cast<void* const*>(env + 0x30) : nullptr;
    FocusLog("frame %llu: console command '%s' (console %p)", static_cast<unsigned long long>(g_frame), command, console);
    if (console)
        reinterpret_cast<void (*)(void*, const char*)>((*reinterpret_cast<void* const* const*>(console))[31])(console, command);
}

// Timedemo recording keys, a beep each (high: started, low: stopped). CRYMT_RECORDKEYS=<demo name>
// (Bin64\CryMT_GI_Record_Rooms.bat): Num1 "record <name>", Num2 "stoprecording", the GI keys off. CRYMT_RECORDKEYS=auto
// (Bin64\CryMT_GI_Test*.bat): Insert and Delete (free in the game's and DevMode's key maps), the GI keys stay; every
// recording gets its own name, rec_MMDD_HHMMSS, so none is overwritten. The engine writes the demo into the loaded
// level's folder (Game\Levels\<level>\<name>.tmd); logs\mt\crymt_records.txt lists them. With auto, PgUp also
// writes the next 8 frames at full size (D3D12 back end: logs\play\key_MMDD_HHMMSS_w1_c<n>.ppm), what the player sees.
char g_recordName[64];
bool g_recordAuto;
bool g_recordKeyDown[2];
bool g_captureKeyDown;

void RecordHotkeys()
{
    if (!g_recordName[0])
        return;
    static const int kNumpad[2] = {VK_NUMPAD1, VK_NUMPAD2}, kAuto[2] = {VK_INSERT, VK_DELETE};
    const int* keys = g_recordAuto ? kAuto : kNumpad;
    const bool focus = GameHasFocus();
    if (g_recordAuto)
    {
        const bool down = focus && (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
        if (down && !g_captureKeyDown)
        {
            Dx12CaptureNow(8, (g_giControl.enabled ? 1 : 0) | (g_giControl.gtao != 0 ? 2 : 0) | (g_giControl.contact != 0 ? 4 : 0) |
                                  ((g_giControl.bakedOff > 0 ? g_giControl.bakedOff.load() : 0) & 30) << 2);
            SYSTEMTIME t;
            GetLocalTime(&t);
            FILE* f = nullptr;
            if (fopen_s(&f, CryMTLog("crymt_records.txt").c_str(), "a") == 0 && f)
            {
                fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u capture 8 frames (logs\\play\\key_*)\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                fclose(f);
            }
            Beep(2000, 80);
        }
        g_captureKeyDown = down;
    }
    for (int i = 0; i < 2; i++)
    {
        const bool down = focus && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        const bool pressed = down && !g_recordKeyDown[i];
        g_recordKeyDown[i] = down;
        if (!pressed)
            continue;
        SYSTEMTIME t;
        GetLocalTime(&t);
        char name[64];
        if (g_recordAuto)
            sprintf_s(name, "rec_%02u%02u_%02u%02u%02u", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        else
            strcpy_s(name, g_recordName);
        char command[96];
        if (i == 0)
            sprintf_s(command, "record %s", name);
        else
            sprintf_s(command, "stoprecording");
        ConsoleCommand(command);
        FILE* f = nullptr;
        if (fopen_s(&f, CryMTLog("crymt_records.txt").c_str(), "a") == 0 && f)
        {
            fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, command);
            fclose(f);
        }
        Beep(i == 0 ? 1200 : 500, 150);
    }
}

void RunTestCommands()
{
    if (!g_testCommands[g_testNext])
        return;
    const DWORD now = GetTickCount();
    if (!g_testLast)
        g_testLast = now;
    if (now - g_testLast < static_cast<DWORD>(g_testInterval) * 1000)
        return;
    g_testLast = now;
    char command[128];
    size_t n = 0;
    while (g_testCommands[g_testNext] && g_testCommands[g_testNext] != ';' && n + 1 < sizeof(command))
        command[n++] = g_testCommands[g_testNext++];
    command[n] = 0;
    if (g_testCommands[g_testNext] == ';')
        g_testNext++;
    ConsoleCommand(command);
}

// GI test keys (CRYMT_GI_KEYS=1, Bin64\CryMT_GI_Test*.bat): the numpad on main while the game has focus (NumLock on);
// the D3D12 back end applies the switches (GiControl.h) and shows them in a panel. Num0 GI, Num1 voxel cones, Num2 voxel
// view (off, wire, solid), Num4/Num7 occlusion under cover -/+, Num5/Num8 bounce light -/+, Num9 fast time of day on/off, Num6
// the start time (CRYMT_GI_STARTTIME, else 11:30) and stopped, Num+/Num- time +/- 1 hour, Num* reset the scales, Num/ panel. CRYMT_GI_STARTTIME sets the
// time once 300 world frames in (a level sets its own after the start configs), CRYMT_GI_TIMESPEED the fast speed in
// game hours per second.
// The time of day goes through ITimeOfDay (gEnv->p3DEngine (+0x40) vtable 161 GetTimeOfDay; SetTime slot 3, GetTime 4,
// Mod SDK order, as e_time_of_day's change callback 0x3012CD50 calls it). The cvar itself keeps the last value set, and
// e_time_of_day_speed (fAnimSpeed) does not run the clock on a level that keeps it paused, so the fast mode moves the
// time itself each frame.
bool g_giKeys;
float g_giStartTime = -1.0f, g_giTimeSpeed = 0.3f;
bool g_giKeyDown[24];
LARGE_INTEGER g_giLastTick;

void* TimeOfDayObject()
{
    const auto* env = *reinterpret_cast<uint8_t* const*>(kCryActionEnv);
    void* engine = env ? *reinterpret_cast<void* const*>(env + 0x40) : nullptr;
    return engine ? reinterpret_cast<void* (*)(void*)>((*reinterpret_cast<void* const* const*>(engine))[161])(engine) : nullptr;
}

float GetTimeOfDay()
{
    void* tod = TimeOfDayObject();
    return tod ? reinterpret_cast<float (*)(void*)>((*reinterpret_cast<void* const* const*>(tod))[4])(tod) : -1.0f;
}

// force: everything recomputed at once (a jump; costs about 15 ms a call), else as the cvar's callback does.
void SetTimeOfDay(float hours, bool force = true)
{
    if (void* tod = TimeOfDayObject())
        reinterpret_cast<void (*)(void*, float, bool)>((*reinterpret_cast<void* const* const*>(tod))[3])(tod, hours, force);
}

void GiHotkeys()
{
    if (!g_giKeys)
        return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = g_giLastTick.QuadPart ? static_cast<float>(now.QuadPart - g_giLastTick.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    g_giLastTick = now;
    if (g_giStartTime >= 0 && g_frame >= 300)
    {
        SetTimeOfDay(g_giStartTime);
        g_giStartTime = -1.0f;
    }
    float hours = GetTimeOfDay();
    if (g_giControl.fastTime && hours >= 0 && dt > 0 && dt < 0.5f)
    {
        hours += g_giTimeSpeed * dt;
        hours = hours >= 24.0f ? hours - 24.0f : hours;
        SetTimeOfDay(hours, false);
    }
    g_giControl.timeOfDay = hours;
    // Home, PgDn, 6 8 9 0 (the main row's digits), "-" and "=": keys the game binds only in multiplayer or not at all.
    static const int kKeys[24] = {VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7,
                                  VK_NUMPAD8, VK_NUMPAD9, VK_MULTIPLY, VK_ADD, VK_SUBTRACT, VK_DIVIDE, VK_DECIMAL,
                                  VK_HOME, VK_NEXT, '6', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_OEM_6};
    const bool focus = GameHasFocus();
    const auto step = [](std::atomic<float>& v, float by, float most) {
        const float n = static_cast<float>(static_cast<int>((v + by) * 10.0f + (v + by >= 0 ? 0.5f : -0.5f))) / 10.0f;
        v = n < 0 ? 0.0f : n > most ? most : n;
    };
    for (int i = 0; i < 24; i++)
    {
        const bool down = focus && (GetAsyncKeyState(kKeys[i]) & 0x8000) != 0;
        const bool pressed = down && !g_giKeyDown[i];
        g_giKeyDown[i] = down;
        if (!pressed)
            continue;
        const float tod = g_giControl.timeOfDay;
        switch (kKeys[i])
        {
        case VK_NUMPAD0: g_giControl.enabled = !g_giControl.enabled; break;
        case VK_NUMPAD1: g_giControl.parts = g_giControl.parts ? 0 : 1; break;
        case VK_DECIMAL: g_giControl.weapon = !g_giControl.weapon; break;
        case VK_NUMPAD2: g_giControl.view = (g_giControl.view + 1) % 3; break;
        case VK_NUMPAD3: g_giControl.geometry = g_giControl.geometry > 0 ? 0 : 1; break;
        case VK_NUMPAD4: step(g_giControl.occlusionScale, -0.1f, 3.0f); break;
        case VK_NUMPAD7: step(g_giControl.occlusionScale, 0.1f, 3.0f); break;
        case VK_NUMPAD5: step(g_giControl.bounceScale, -0.1f, 4.0f); break;
        case VK_NUMPAD8: step(g_giControl.bounceScale, 0.1f, 4.0f); break;
        case VK_NUMPAD9: g_giControl.fastTime = !g_giControl.fastTime; break;
        case VK_NUMPAD6:
            g_giControl.fastTime = false;
            SetTimeOfDay(g_giControl.startTime);
            break;
        case VK_ADD:
        case VK_SUBTRACT:
            if (tod >= 0)
            {
                const float t = tod + (kKeys[i] == VK_ADD ? 1.0f : -1.0f);
                SetTimeOfDay(t >= 24.0f ? t - 24.0f : t < 0.0f ? t + 24.0f : t);
            }
            break;
        case VK_MULTIPLY:
            g_giControl.occlusionScale = 1.0f;
            g_giControl.bounceScale = 1.0f;
            break;
        case VK_DIVIDE: g_giControl.panel = !g_giControl.panel; break;
        case VK_HOME: g_giControl.contact = g_giControl.contact != 0 ? 0 : 1; break; // -1: the default (on) not yet set
        case VK_NEXT: g_giControl.gtao = g_giControl.gtao != 0 ? 0 : 1; break;
        case VK_OEM_MINUS: g_giControl.particles = g_giControl.particles != 0 ? 0 : 1; break; // -1: the default (on) not yet set
        case VK_OEM_6: g_giControl.fixCopies = g_giControl.fixCopies ? 0 : 1; break; // "]" test: DLSS into the scene's copies (F2: debug teleport)
        case VK_OEM_PLUS: // "=": the particles' darkening outdoors 50 -> 70 -> 100 -> 150 -> 200 -> 50 %
        {
            const int v = g_giControl.particleOut;
            g_giControl.particleOut = v < 70 ? 70 : v < 100 ? 100 : v < 150 ? 150 : v < 200 ? 200 : 50;
            break;
        }
        case '6':
        case '8':
        case '9':
        case '0':
        {
            const int bit = kKeys[i] == '6' ? 2 : kKeys[i] == '8' ? 4 : kKeys[i] == '9' ? 8 : 16;
            const int v = g_giControl.bakedOff;
            g_giControl.bakedOff = (v < 0 ? 0 : v) ^ bit;
            break;
        }
        default: break;
        }
        FocusLog("frame %llu: GI key %d", static_cast<unsigned long long>(g_frame), kKeys[i]);
    }
}

// CRYMT_GI_TIMESWEEP=from,to,frames (tests): the time of day goes from `from` to `to` hours over world frames 0..frames
// (then stays), by the frame counter, so every run of a timedemo has the same time at the same frame.
void TimeSweep()
{
    static int state = 0; // 0 unread, 1 on, 2 off
    static float from, to, frames;
    if (state == 0)
    {
        char text[64] = {};
        state = GetEnvironmentVariableA("CRYMT_GI_TIMESWEEP", text, sizeof(text)) && sscanf_s(text, "%f,%f,%f", &from, &to, &frames) == 3 && frames > 0 ? 1 : 2;
    }
    if (state != 1)
        return;
    const float t = static_cast<float>(g_frame) / frames;
    SetTimeOfDay(from + (to - from) * (t > 1.0f ? 1.0f : t), false);
}

// CRYMT_CAPTURE_D3D10=1 (tests on the D3D10 path: CRYMT_MODE=0 for the stock renderer, or mode 2 without CRYMT_D3D12):
// the world frames CRYMT_CAPTURE_FRAME lists, CRYMT_CAPTURE_COUNT from each (8), read back at the swap chain's Present
// into logs\play\<CRYMT_CAPTURE_TAG>_w<window>_c<n>.ppm, as the D3D12 back end's capture names them: stock references
// for the same timedemo frames.
using Present10Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
uintptr_t g_origPresent10;
struct Capture10
{
    std::vector<uint64_t> starts;
    size_t window = 0;
    int count = 8, done = 0;
    char tag[64] = "capture";
};
Capture10 g_cap10;

void Capture10Write(IDXGISwapChain* sc, size_t window, int index)
{
    ID3D10Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&back))) || !back)
        return;
    D3D10_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    ID3D10Device* dev = nullptr;
    back->GetDevice(&dev);
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    d.Usage = D3D10_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D10Texture2D* staging = nullptr;
    if (dev && d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging)) && staging)
    {
        dev->CopyResource(staging, back);
        D3D10_MAPPED_TEXTURE2D m{};
        if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &m)))
        {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s%s_w%zu_c%d.ppm", CryMTCapture("").c_str(), g_cap10.tag, window + 1, index + 1);
            FILE* f = nullptr;
            if (fopen_s(&f, path, "wb") == 0 && f)
            {
                fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
                std::vector<uint8_t> row(static_cast<size_t>(d.Width) * 3);
                for (UINT y = 0; y < d.Height; y++)
                {
                    const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                    for (UINT x = 0; x < d.Width; x++)
                    {
                        row[x * 3] = src[x * 4 + (bgra ? 2 : 0)];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + (bgra ? 0 : 2)];
                    }
                    fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f);
            }
            staging->Unmap(0);
        }
        staging->Release();
    }
    if (dev)
        dev->Release();
    back->Release();
}

HRESULT STDMETHODCALLTYPE Present10Hook(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    Capture10& c = g_cap10;
    if (c.window < c.starts.size() && g_frame >= c.starts[c.window])
    {
        Capture10Write(sc, c.window, c.done);
        if (++c.done >= c.count)
        {
            c.done = 0;
            c.window++;
        }
    }
    return reinterpret_cast<Present10Fn>(g_origPresent10)(sc, sync, flags);
}

void Capture10Install()
{
    static int state = 0; // 0 unread, 1 waiting for the swap chain, 2 done or off
    if (state == 2)
        return;
    if (state == 0)
    {
        char text[256] = {};
        if (!GetEnvironmentVariableA("CRYMT_CAPTURE_D3D10", text, sizeof(text)) || strcmp(text, "0") == 0 ||
            !GetEnvironmentVariableA("CRYMT_CAPTURE_FRAME", text, sizeof(text)))
        {
            state = 2;
            return;
        }
        for (const char* q = text; *q;)
        {
            g_cap10.starts.push_back(static_cast<uint64_t>(atof(q)));
            while (*q && *q != ',')
                q++;
            if (*q == ',')
                q++;
        }
        if (GetEnvironmentVariableA("CRYMT_CAPTURE_COUNT", text, sizeof(text)))
            g_cap10.count = atoi(text);
        if (GetEnvironmentVariableA("CRYMT_CAPTURE_TAG", text, sizeof(text)))
            strcpy_s(g_cap10.tag, text);
        state = 1;
    }
    const auto* env = *reinterpret_cast<uint8_t* const*>(kCryActionEnv);
    const uintptr_t renderer = env ? *reinterpret_cast<const uintptr_t*>(env + 0x18) : 0; // gEnv->pRenderer
    auto* unk = renderer ? *reinterpret_cast<IUnknown**>(renderer + kSwapChainOffset) : nullptr;
    IDXGISwapChain* sc = nullptr;
    if (!unk || FAILED(unk->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc))) || !sc)
        return;
    PatchVtableSlot(*reinterpret_cast<uintptr_t*>(sc), 8, &g_origPresent10, reinterpret_cast<const void*>(&Present10Hook));
    sc->Release();
    SetupLog("capture: D3D10 Present hooked");
    state = 2;
}

bool HookPreUpdate(void* framework, bool engineFocus, unsigned int flags)
{
    RunTestCommands();
    RecordHotkeys();
    GiHotkeys();
    TimeSweep();
    Capture10Install();
    static const uint64_t keyAt = [] { // CRYMT_CAPTURE_KEYAT=<world frame> (tests): the capture key pressed at that frame
        char text[32] = {};
        return GetEnvironmentVariableA("CRYMT_CAPTURE_KEYAT", text, sizeof(text)) ? static_cast<uint64_t>(_atoi64(text)) : 0ull;
    }();
    if (keyAt && g_frame == keyAt)
    {
        Dx12CaptureNow(8, 7);
        rdoc::Trigger();
    }
    rdoc::Poll(GameHasFocus());
    if (g_dlssOn)
        SnapshotLamps();
    if (!g_pauseOnFocus)
        return g_origPreUpdate(framework, engineFocus, flags);
    const bool haveFocus = GameHasFocus();
    if (haveFocus != g_hadFocus)
        FocusLog("frame %llu: focus %d", static_cast<unsigned long long>(g_frame), haveFocus);
    if (!haveFocus && g_hadFocus && g_input)
    {
        auto* const* slots = *reinterpret_cast<void* const* const*>(framework);
        const bool started = reinterpret_cast<FrameworkQueryFn>(slots[15])(framework);
        const bool paused = reinterpret_cast<FrameworkQueryFn>(slots[14])(framework);
        FocusLog("  game started %d, paused %d", started, paused);
        if (started && !paused)
        {
            const auto post = reinterpret_cast<PostInputEventFn>((*reinterpret_cast<void* const* const*>(g_input))[7]);
            InputEvent e{0, 1, "escape", 0, GetTickCount(), 0, 1.0f, nullptr}; // keyboard, pressed, eKI_Escape
            post(g_input, e);
            e.state = 2; // released
            e.value = 0.0f;
            post(g_input, e);
            g_focusPauses++;
        }
    }
    g_hadFocus = haveFocus;
    return g_origPreUpdate(framework, engineFocus, flags);
}

// Save file written on a worker (CRYMT_ASYNCSAVE, default on). A save captures the game state into an XML tree on main
// (entities, AI, ...: ~155 ms on harbor) and then calls the save object's Complete(true), which converts the tree to text,
// compresses it, writes the file through ICryPak and deletes the object (~110 ms of the ~270 ms freeze). Nothing touches
// the object or the file after that call, so the call site hands the object to a worker and reports success. The next
// save, LoadGame and process exit wait for the write. A failed write is logged (the game had already shown success).
using SaveCompleteFn = bool(__fastcall*)(void*, bool);

DWORD WINAPI SaveWriterMain(void* save)
{
    const int64_t start = Now();
    const bool ok = reinterpret_cast<SaveCompleteFn>((*static_cast<void***>(save))[kSaveCompleteSlot])(save, true);
    g_saveWriteTicks += Now() - start;
    g_saveWrites++;
    if (!ok)
    {
        g_saveWriteFailures++;
        SetupLog("async save: the write failed");
    }
    return ok ? 0 : 1;
}

void WaitSaveWriter(DWORD timeoutMs)
{
    if (HANDLE h = g_saveWriter.exchange(nullptr))
    {
        g_saveWaits += WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        WaitForSingleObject(h, timeoutMs);
        CloseHandle(h);
    }
}

extern "C" bool SaveCompleteHook(void* save)
{
    WaitSaveWriter(INFINITE); // one write at a time (a quicksave may rewrite the file of the previous one)
    HANDLE h = CreateThread(nullptr, 0, SaveWriterMain, save, 0, nullptr);
    if (!h)
        return reinterpret_cast<SaveCompleteFn>((*static_cast<void***>(save))[kSaveCompleteSlot])(save, true);
    g_saveWriter = h;
    return true;
}

using LoadGameFn = uint64_t(__fastcall*)(void*, uint64_t, uint64_t, uint64_t);
LoadGameFn g_origLoadGame;
uint64_t __fastcall HookLoadGame(void* framework, uint64_t path, uint64_t quick, uint64_t ignoreDelay)
{
    WaitSaveWriter(INFINITE);
    return g_origLoadGame(framework, path, quick, ignoreDelay);
}

using ExitProcessFn = void(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
ExitProcessFn g_origExitProcess;
TerminateProcessFn g_origTerminateProcess;
void WINAPI HookExitProcess(UINT code)
{
    WaitSaveWriter(10000);
    g_origExitProcess(code);
}
BOOL WINAPI HookTerminateProcess(HANDLE process, UINT code)
{
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        WaitSaveWriter(10000);
    return g_origTerminateProcess(process, code);
}

// The call sequence is replaced by "mov rcx,rbx; call cave" (the cave jumps to SaveCompleteHook, whose bool lands in al
// as Complete's did). CryAction has no ASLR, so the cave is allocated within rel32 reach above the module.
void InstallAsyncSave(uintptr_t base)
{
    wchar_t value[8] = {};
    if (GetEnvironmentVariableW(L"CRYMT_ASYNCSAVE", value, 8) && wcscmp(value, L"0") == 0)
        return;
    auto* site = reinterpret_cast<uint8_t*>(kSaveCompleteSite);
    if (memcmp(site, kSaveCompleteCode, sizeof(kSaveCompleteCode)) != 0)
    {
        SetupLog("async save: CryAction code differs, off");
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uint8_t* cave = nullptr;
    for (uintptr_t hint = base + nt->OptionalHeader.SizeOfImage + 0x100000; hint < base + 0x40000000 && !cave; hint += 0x10000)
        cave = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!cave)
    {
        SetupLog("async save: no code cave, off");
        return;
    }
    cave[0] = 0x48; // movabs rax, SaveCompleteHook
    cave[1] = 0xB8;
    const auto hook = reinterpret_cast<uint64_t>(&SaveCompleteHook);
    memcpy(cave + 2, &hook, 8);
    cave[10] = 0xFF; // jmp rax
    cave[11] = 0xE0;
    DWORD old;
    VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &old);
    VirtualProtect(site, sizeof(kSaveCompleteCode), PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0x48; // mov rcx, rbx
    site[1] = 0x8B;
    site[2] = 0xCB;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(cave) - reinterpret_cast<intptr_t>(site + 8));
    site[3] = 0xE8;
    memcpy(site + 4, &rel, 4);
    memset(site + 8, 0x90, sizeof(kSaveCompleteCode) - 8);
    VirtualProtect(site, sizeof(kSaveCompleteCode), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kSaveCompleteCode));
    // Quitting while a write runs would cut the file short: the game ends with TerminateProcess (CrySystem) or
    // ExitProcess (the CRT, the launcher).
    HMODULE kernel = GetModuleHandleW(L"KERNEL32.dll");
    g_origExitProcess = reinterpret_cast<ExitProcessFn>(GetProcAddress(kernel, "ExitProcess"));
    g_origTerminateProcess = reinterpret_cast<TerminateProcessFn>(GetProcAddress(kernel, "TerminateProcess"));
    static const char* const names[] = {"ExitProcess", "TerminateProcess"};
    static const void* const hooks[] = {reinterpret_cast<const void*>(&HookExitProcess), reinterpret_cast<const void*>(&HookTerminateProcess)};
    int patched = 0;
    for (const wchar_t* module : {static_cast<const wchar_t*>(nullptr), L"CrySystem.dll", L"MSVCR80.dll", L"CryGame.dll", L"CryAction.dll"})
        if (HMODULE m = module ? GetModuleHandleW(module) : GetModuleHandleW(nullptr))
            patched += CryMTPatchImports(reinterpret_cast<uintptr_t>(m), "KERNEL32.dll", names, hooks, 2);
    g_saveWriterOn = true;
    char line[96];
    sprintf_s(line, "async save: on (exit imports patched %d)", patched);
    SetupLog(line);
}

void* HookCreateGameFramework(void* a, void* b, void* c, void* d)
{
    void* framework = g_origCreateFramework(a, b, c, d);
    if (framework && !g_origLoadGame && g_saveWriterOn)
        PatchVtableSlot(**reinterpret_cast<uintptr_t**>(&framework), kFrameworkLoadGameSlot, reinterpret_cast<uintptr_t*>(&g_origLoadGame),
                        reinterpret_cast<const void*>(&HookLoadGame));
    if (framework && !g_origPreUpdate)
        PatchVtableSlot(**reinterpret_cast<uintptr_t**>(&framework), 9, reinterpret_cast<uintptr_t*>(&g_origPreUpdate),
                        reinterpret_cast<const void*>(&HookPreUpdate));
    FocusLog("framework %p, vtable %p, PreUpdate %p", framework, framework ? *reinterpret_cast<void**>(framework) : nullptr,
             reinterpret_cast<void*>(g_origPreUpdate));
    return framework;
}

void* HookCreateInput(void* a, void* b, void* c, void* d)
{
    g_input = g_origCreateInput(a, b, c, d);
    FocusLog("input %p", g_input);
    return g_input;
}

// CryAction and CryInput of build 6156; another build keeps the feature off (CryMT itself still runs).
void InstallPauseOnFocus(const UNICODE_STRING* n, uintptr_t base)
{
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const DWORD stamp = nt->FileHeader.TimeDateStamp;
    if (NameIs(n, L"CryAction.dll") && stamp == 0x47D6DE82)
        InstallAsyncSave(base);
    if (NameIs(n, L"CryAction.dll") && stamp == 0x47D6DE82)
        g_origCreateFramework = reinterpret_cast<CreateExportFn>(PatchModuleExport(base, "CreateGameFramework", reinterpret_cast<const void*>(&HookCreateGameFramework)));
    else if (NameIs(n, L"CryInput.dll") && stamp == 0x47D6E17A)
        g_origCreateInput = reinterpret_cast<CreateExportFn>(PatchModuleExport(base, "CreateInput", reinterpret_cast<const void*>(&HookCreateInput)));
}

// More back buffers (CRYMT_BUFFERS). The renderer creates its swap chain with BufferCount 1, so when the GPU
// is the limit it waits inside Present for the presented buffer before it can start the next frame
// (0.2 ms of GPU time in Present in calm frames, 1.7 ms in explosions at 5120x1440). The renderer gets
// CreateDXGIFactory through GetProcAddress, so the dxgi export is redirected and the factory's
// CreateSwapChain (vtable slot 10) and the swap chain's ResizeBuffers (slot 13) raise the count.
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
constexpr int kCreateSwapChainSlot = 10;
constexpr int kResizeBuffersSlot = 13;
UINT g_backBuffers;
bool g_dxgiPatched;
CreateFactoryFn g_origCreateFactory;
CreateSwapChainFn g_origCreateSwapChain;
ResizeBuffersFn g_origResizeBuffers;

void PatchVtableSlot(uintptr_t vtable, int slot, uintptr_t* original, const void* hook);

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    if (count != 0 && count < g_backBuffers)
        count = g_backBuffers;
    return g_origResizeBuffers(sc, count, w, h, fmt, flags);
}

// Borderless fullscreen on the D3D10 swap chain (Window.h): the chain stays windowed, the game is told what it asked for.
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using GetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
using ResizeTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
SetFullscreenStateFn g_origSetFullscreenState;
GetFullscreenStateFn g_origGetFullscreenState;
ResizeTargetFn g_origResizeTarget;

HRESULT STDMETHODCALLTYPE HookSetFullscreenState(IDXGISwapChain* sc, BOOL fullscreen, IDXGIOutput*)
{
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    BorderlessSetFullscreen(d.OutputWindow, fullscreen != FALSE);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookGetFullscreenState(IDXGISwapChain*, BOOL* fullscreen, IDXGIOutput** target)
{
    if (fullscreen)
        *fullscreen = BorderlessFullscreen();
    if (target)
        *target = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* mode)
{
    return BorderlessFullscreen() ? S_OK : g_origResizeTarget(sc, mode);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    DXGI_SWAP_CHAIN_DESC d = *desc;
    d.BufferCount = std::max(d.BufferCount, g_backBuffers);
    const bool borderless = BorderlessOn();
    if (borderless)
        d.Windowed = TRUE;
    HRESULT hr = g_origCreateSwapChain(factory, device, &d, out);
    if (FAILED(hr) && d.BufferCount != desc->BufferCount)
    {
        d.BufferCount = desc->BufferCount;
        hr = g_origCreateSwapChain(factory, device, &d, out);
    }
    if (SUCCEEDED(hr) && *out && !g_origResizeBuffers)
        PatchVtableSlot(**reinterpret_cast<uintptr_t**>(out), kResizeBuffersSlot, reinterpret_cast<uintptr_t*>(&g_origResizeBuffers),
                        reinterpret_cast<const void*>(&HookResizeBuffers));
    if (SUCCEEDED(hr) && *out && borderless)
    {
        if (!g_origSetFullscreenState)
        {
            const uintptr_t table = **reinterpret_cast<uintptr_t**>(out);
            PatchVtableSlot(table, 10, reinterpret_cast<uintptr_t*>(&g_origSetFullscreenState), reinterpret_cast<const void*>(&HookSetFullscreenState));
            PatchVtableSlot(table, 11, reinterpret_cast<uintptr_t*>(&g_origGetFullscreenState), reinterpret_cast<const void*>(&HookGetFullscreenState));
            PatchVtableSlot(table, 14, reinterpret_cast<uintptr_t*>(&g_origResizeTarget), reinterpret_cast<const void*>(&HookResizeTarget));
        }
        factory->MakeWindowAssociation(d.OutputWindow, DXGI_MWA_NO_ALT_ENTER);
        BorderlessSetFullscreen(d.OutputWindow, !desc->Windowed);
    }
    char line[96];
    sprintf_s(line, "swap chain created: buffers %u -> %u hr=%lx", desc->BufferCount, SUCCEEDED(hr) ? d.BufferCount : desc->BufferCount,
              static_cast<unsigned long>(hr));
    SetupLog(line);
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory(REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory(riid, out);
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(hr) && *out && !g_origCreateSwapChain &&
        SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
    {
        PatchVtableSlot(*reinterpret_cast<uintptr_t*>(factory), kCreateSwapChainSlot, reinterpret_cast<uintptr_t*>(&g_origCreateSwapChain),
                        reinterpret_cast<const void*>(&HookCreateSwapChain));
        factory->Release();
    }
    return hr;
}

void PatchDxgi(uintptr_t dxgi)
{
    if (g_dxgiPatched || (!g_backBuffers && !BorderlessOn()) || !dxgi)
        return;
    g_dxgiPatched = true;
    g_origCreateFactory = reinterpret_cast<CreateFactoryFn>(
        PatchModuleExport(dxgi, "CreateDXGIFactory", reinterpret_cast<const void*>(&HookCreateDXGIFactory)));
    SetupLog(g_origCreateFactory ? "dxgi CreateDXGIFactory redirected" : "dxgi CreateDXGIFactory not found");
}

void InstallDeferredFree()
{
    const auto exe = GetModuleHandleW(nullptr);
    g_origCryMalloc = reinterpret_cast<CryMallocFn>(GetProcAddress(exe, "CryMalloc"));
    g_origCryGetMemSize = reinterpret_cast<CryGetMemSizeFn>(GetProcAddress(exe, "CryGetMemSize"));
    if (!g_origCryMalloc || !g_origCryGetMemSize)
    {
        SetupLog("EXE does not export CryMalloc, deferred free not installed");
        return;
    }
    g_origCryFree = reinterpret_cast<CryFreeFn>(PatchExeExport("CryFree", reinterpret_cast<const void*>(&HookCryFree)));
    g_origCryRealloc = reinterpret_cast<CryReallocFn>(PatchExeExport("CryRealloc", reinterpret_cast<const void*>(&HookCryRealloc)));
    g_origCrtFree = reinterpret_cast<CrtFreeFn>(PatchExeExport("CrySystemCrtFree", reinterpret_cast<const void*>(&HookCrtFree)));
    char line[96];
    sprintf_s(line, "deferred free installed: free=%d realloc=%d crtfree=%d", g_origCryFree != nullptr,
              g_origCryRealloc != nullptr, g_origCrtFree != nullptr);
    SetupLog(line);
}

} // namespace

extern "C" void __cdecl FenceHook(int reason)
{
    if (g_mode == 2 && g_async && GetCurrentThreadId() == g_mainThread)
        WaitIdlePublic(reason);
}

namespace
{
// Generates "call hook(arg), then jump to original" with all argument registers kept.
void* MakeHookThunk(uintptr_t original, void (*hookFn)(int), int arg)
{
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint8_t* c = p;
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes)
            *c++ = b;
    };
    put({0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x68});  // push rcx,rdx,r8,r9; sub rsp,0x68
    put({0x0F, 0x11, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x4C, 0x24, 0x30}); // movups [rsp+0x20],xmm0; [rsp+0x30],xmm1
    put({0x0F, 0x11, 0x54, 0x24, 0x40, 0x0F, 0x11, 0x5C, 0x24, 0x50}); // movups [rsp+0x40],xmm2; [rsp+0x50],xmm3
    put({0xB9});                                                        // mov ecx, arg
    memcpy(c, &arg, 4);
    c += 4;
    put({0x48, 0xB8});                                                  // movabs rax, hook
    const auto hook = reinterpret_cast<uint64_t>(hookFn);
    memcpy(c, &hook, 8);
    c += 8;
    put({0xFF, 0xD0});                                                  // call rax
    put({0x0F, 0x10, 0x44, 0x24, 0x20, 0x0F, 0x10, 0x4C, 0x24, 0x30});
    put({0x0F, 0x10, 0x54, 0x24, 0x40, 0x0F, 0x10, 0x5C, 0x24, 0x50});
    put({0x48, 0x83, 0xC4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59}); // add rsp,0x68; pop r9,r8,rdx,rcx
    put({0x48, 0xB8});                                                  // movabs rax, original
    memcpy(c, &original, 8);
    c += 8;
    put({0xFF, 0xE0});                                                  // jmp rax
    DWORD old;
    VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
    return p;
}

// Generates "wait for the render thread on main, then jump to original".
void* MakeFenceThunk(uintptr_t original, int reason)
{
    return MakeHookThunk(original, &FenceHook, reason);
}

// Puts a fence in front of the function referenced by a vtable slot (an absolute address in .rdata).
void FenceVtableEntry(uintptr_t slotAddress, uintptr_t expected, int reason)
{
    auto* slot = reinterpret_cast<uintptr_t*>(slotAddress);
    if (*slot != expected)
        return;
    void* thunk = MakeFenceThunk(expected, reason);
    DWORD old;
    VirtualProtect(slot, 8, PAGE_READWRITE, &old);
    *slot = reinterpret_cast<uintptr_t>(thunk);
    VirtualProtect(slot, 8, old, &old);
}

void PatchVtableSlot(uintptr_t vtable, int slot, uintptr_t* original, const void* hook)
{
    auto* entry = reinterpret_cast<uintptr_t*>(vtable) + slot;
    if (original)
        *original = *entry;
    DWORD old;
    VirtualProtect(entry, 8, PAGE_READWRITE, &old);
    *entry = reinterpret_cast<uintptr_t>(hook);
    VirtualProtect(entry, 8, old, &old);
}

void PatchSkinRelease()
{
    auto* charVt = reinterpret_cast<uintptr_t*>(kCharInstanceVtable);
    auto* skinVt = reinterpret_cast<uintptr_t*>(kSkinInstanceVtable);
    if (charVt[kReleaseSlot] != kSkinRelease || skinVt[kReleaseSlot] != kSkinRelease || skinVt[kClearModelSlot] != 0x315257A0 ||
        charVt[kClearModelSlot] != 0x3151C320 ||
        charVt[kDeletingDtorSlot] != 0x31519060 || skinVt[kDeletingDtorSlot] != 0x315257B0)
    {
        SetupLog("character vtables differ from build 6156, not patched");
        return;
    }
    PatchVtableSlot(kCharInstanceVtable, kReleaseSlot, nullptr, reinterpret_cast<const void*>(&DeferredSkinRelease));
    PatchVtableSlot(kSkinInstanceVtable, kReleaseSlot, nullptr, reinterpret_cast<const void*>(&DeferredSkinRelease));
    PatchVtableSlot(kCharInstanceVtable, kDeletingDtorSlot, &g_charDeletingDtor, reinterpret_cast<const void*>(&DeferredCharDelete));
    PatchVtableSlot(kSkinInstanceVtable, kDeletingDtorSlot, &g_skinDeletingDtor, reinterpret_cast<const void*>(&DeferredSkinDelete));
    PatchVtableSlot(kSkinInstanceVtable, kClearModelSlot, &g_skinClearModel, reinterpret_cast<const void*>(&DeferredSkinClearModel));
    PatchVtableSlot(kCharInstanceVtable, kClearModelSlot, &g_charClearModel, reinterpret_cast<const void*>(&DeferredCharClearModel));
    PatchVtableSlot(kCharInstanceVtable, kSkeletonPoseSlot, &g_charGetPose, reinterpret_cast<const void*>(&HookGetPose));
    PatchVtableSlot(kSkinInstanceVtable, kSkeletonPoseSlot, &g_skinGetPose, reinterpret_cast<const void*>(&HookGetPose));
    SetupLog("character Release, deleting destructor and model clear patched");
}

// Particles: the flush builds vertices from containers (IParticleVertexCreator::SetVertices, 3D+0x84AE0, which
// runs CParticleContainer::UpdateParticles under the container's lock), while main destroys emitters and
// containers, e.g. dead emitters in CPartManager::Update (FUN_3007CFA0) at the start of RenderWorld. Both are
// _reference_target based: Release (slot 2) calls the scalar deleting destructor (slot 0), queued here like the
// character ones until the render thread is idle.
uintptr_t g_emitterDelete;
uintptr_t g_containerDelete;

extern "C" void* __fastcall DeferredEmitterDelete(void* self, uint64_t flags)
{
    if (DeferCharacterOpsNow())
    {
        g_pendingReleases.push_back({g_emitterDelete, self, flags});
        g_particleDeferred++;
        return self;
    }
    return reinterpret_cast<void*(__fastcall*)(void*, uint64_t)>(g_emitterDelete)(self, flags);
}

extern "C" void* __fastcall DeferredContainerDelete(void* self, uint64_t flags)
{
    if (DeferCharacterOpsNow())
    {
        g_pendingReleases.push_back({g_containerDelete, self, flags});
        g_particleDeferred++;
        return self;
    }
    return reinterpret_cast<void*(__fastcall*)(void*, uint64_t)>(g_containerDelete)(self, flags);
}

void PatchParticleDelete()
{
    if (reinterpret_cast<const uintptr_t*>(kEmitterVtable)[0] != kEmitterDeletingDtor ||
        reinterpret_cast<const uintptr_t*>(kContainerVtable)[0] != kContainerDeletingDtor)
    {
        SetupLog("particle vtables differ from build 6156, not patched");
        return;
    }
    PatchVtableSlot(kEmitterVtable, 0, &g_emitterDelete, reinterpret_cast<const void*>(&DeferredEmitterDelete));
    PatchVtableSlot(kContainerVtable, 0, &g_containerDelete, reinterpret_cast<const void*>(&DeferredContainerDelete));
    SetupLog("particle emitter and container destruction deferred");
}

// Engine locks held by main (fill mode): Cry3DEngine's imports of Enter/TryEnter/LeaveCriticalSection are
// counted for the main thread. Main waiting for the render thread while it holds one can deadlock, e.g. a
// particle container lock (container +0x110) that the flush takes in SetVertices -> UpdateParticles.
using CsFn = void(WINAPI*)(LPCRITICAL_SECTION);
using TryCsFn = BOOL(WINAPI*)(LPCRITICAL_SECTION);
// The particle system takes container locks in opposite orders on main (CPartManager::Update: parent, then the
// children, 3D+0x6C4F0) and in the flush (SetVertices -> UpdateParticles of a child, then of its parent at
// 3D+0x70DEA): with both running at once they deadlocked (watchdog). Main and the render thread therefore take
// one engine lock before their outermost Cry3DEngine critical section, so only one of them is inside the
// engine's locked regions at a time. Main never waits for the render thread inside them (counted below).
SRWLOCK g_engineLock = SRWLOCK_INIT;
thread_local int t_engineLocks;

bool EngineLockThread()
{
    const DWORD tid = GetCurrentThreadId();
    return tid == g_mainThread || tid == g_rtTid;
}

void WINAPI EngineEnterCs(LPCRITICAL_SECTION cs)
{
    const bool ours = EngineLockThread();
    if (ours && t_engineLocks++ == 0 && !TryAcquireSRWLockExclusive(&g_engineLock))
    {
        g_engineLockWaits++;
        AcquireSRWLockExclusive(&g_engineLock);
    }
    EnterCriticalSection(cs);
    if (ours && GetCurrentThreadId() == g_mainThread)
        g_mainEngineLocks++;
}

BOOL WINAPI EngineTryEnterCs(LPCRITICAL_SECTION cs)
{
    const bool ours = EngineLockThread();
    if (ours && t_engineLocks == 0 && !TryAcquireSRWLockExclusive(&g_engineLock))
        return FALSE;
    const BOOL got = TryEnterCriticalSection(cs);
    if (ours)
    {
        if (got)
        {
            t_engineLocks++;
            if (GetCurrentThreadId() == g_mainThread)
                g_mainEngineLocks++;
        }
        else if (t_engineLocks == 0)
            ReleaseSRWLockExclusive(&g_engineLock);
    }
    return got;
}

void WINAPI EngineLeaveCs(LPCRITICAL_SECTION cs)
{
    LeaveCriticalSection(cs);
    if (!EngineLockThread() || t_engineLocks == 0) // entered before the imports were patched
        return;
    if (GetCurrentThreadId() == g_mainThread && g_mainEngineLocks > 0)
        g_mainEngineLocks--;
    if (--t_engineLocks == 0)
        ReleaseSRWLockExclusive(&g_engineLock);
}

// Called by WaitIdle on main before it waits.
void NoteSyncUnderLock(int reason)
{
    if (g_mainEngineLocks <= 0)
        return;
    g_syncUnderLock++;
    for (int i = 0; i < 8; i++)
        if (!g_syncUnderLockCount[i] || g_syncUnderLockReason[i] == reason)
        {
            g_syncUnderLockReason[i] = reason;
            g_syncUnderLockCount[i]++;
            break;
        }
}

// WaitIdle on main inside an engine critical section: the flush may need another engine lock (particle
// vertices), so the shared lock is let go while main waits; main takes no locks meanwhile, so no new ordering.
// The engine's own critical section stays held: the flush needing that same one would still deadlock.
bool ReleaseEngineLockForWait()
{
    if (t_engineLocks == 0 || GetCurrentThreadId() != g_mainThread)
        return false;
    ReleaseSRWLockExclusive(&g_engineLock);
    return true;
}

void RetakeEngineLock()
{
    AcquireSRWLockExclusive(&g_engineLock);
}

// Rewrites a module's import address table entries; after load, when the loader has bound them.
int PatchImports(uintptr_t base, const char* dll, const char* const* names, const void* const* hooks, int count)
{
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); dir.VirtualAddress && imp->Name; imp++)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0)
            continue;
        auto* names_ = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; names_->u1.AddressOfData; names_++, iat++)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names_->u1.Ordinal))
                continue;
            const char* fn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names_->u1.AddressOfData)->Name;
            for (int i = 0; i < count; i++)
                if (strcmp(fn, names[i]) == 0)
                {
                    DWORD old;
                    VirtualProtect(&iat->u1.Function, 8, PAGE_READWRITE, &old);
                    iat->u1.Function = reinterpret_cast<ULONGLONG>(hooks[i]);
                    VirtualProtect(&iat->u1.Function, 8, old, &old);
                    patched++;
                }
        }
    }
    return patched;
}
} // namespace

// Export for D3DLayer.cpp.
int CryMTPatchImports(uintptr_t base, const char* dll, const char* const* names, const void* const* hooks, int count)
{
    return PatchImports(base, dll, names, hooks, count);
}

namespace
{
void TrackEngineLocks()
{
    static bool done;
    if (done)
        return;
    done = true;
    static const char* const names[] = {"EnterCriticalSection", "TryEnterCriticalSection", "LeaveCriticalSection"};
    static const void* const hooks[] = {reinterpret_cast<const void*>(&EngineEnterCs), reinterpret_cast<const void*>(&EngineTryEnterCs),
                                        reinterpret_cast<const void*>(&EngineLeaveCs)};
    char line[64];
    sprintf_s(line, "engine lock tracking: %d imports patched",
              PatchImports(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Cry3DEngine.dll")), "KERNEL32.dll", names, hooks, 3));
    SetupLog(line);
}

// RenderWorld's inner render (FUN_3010AAC0) zeroes 3D engine cvars around the pass for each flag bit it lacks;
// the flush reads some of them. The flags of the passes are counted here (fill mode) to see which apply.
using RenderInnerFn = void(__fastcall*)(void*, uint32_t, void*, void*, uint32_t, uint32_t);

extern "C" void __fastcall RenderInnerHook(void* self, uint32_t a, void* camera, void* d, uint32_t flags, uint32_t f)
{
    for (int i = 0; i < 8; i++)
        if (g_innerFlagCount[i] == 0 || g_innerFlagValue[i] == flags)
        {
            g_innerFlagValue[i] = flags;
            g_innerFlagCount[i]++;
            break;
        }
    // The main view passes all flags (0xFFFFFFFF, measured), so nothing is zeroed; secondary passes (0xC32 in
    // about 5% of frames) zero cvars the flush may read, so they wait for it.
    if (flags != 0xFFFFFFFFu && GetCurrentThreadId() == g_mainThread)
        WaitIdlePublic(431);
    reinterpret_cast<RenderInnerFn>(kRenderInnerFn)(self, a, camera, d, flags, f);
}

VOID CALLBACK OnDll(ULONG reason, const LdrNotification* data, PVOID)
{
    if (reason != 1 || g_off)
        return;
    const UNICODE_STRING* n = data->baseName;
    if (NameIs(n, L"CryRenderD3D9.dll"))
    {
        TurnOff("CryMT off: the game runs the DX9 renderer (start it with -dx10)");
        return;
    }
    if (!ModuleMatches(n, reinterpret_cast<uintptr_t>(data->base)))
        return;
    // CryAction (framework: pause on focus loss, test commands, the async save's LoadGame wait; the async save itself) and
    // CryInput hooks; each one is inert when its feature is off.
    InstallPauseOnFocus(n, reinterpret_cast<uintptr_t>(data->base));
    if (NameIs(n, L"Cry3DEngine.dll"))
        zpass::OnEngineLoaded();
    D3DLayerOnDll(n->Buffer, n->Length / 2, reinterpret_cast<uintptr_t>(data->base));
    if (g_mode == 2 && n->Length == 13 * 2 && _wcsnicmp(n->Buffer, L"CrySystem.dll", 13) == 0)
    {
        wchar_t mask[16] = {};
        const int fences = GetEnvironmentVariableW(L"CRYMT_FENCE", mask, 16) ? _wtoi(mask) : 0;
        if (fences & 512)
            FenceVtableEntry(0x3676AC30, 0x36558250, 410); // CTimer slot 1, frame start update
        else if (reinterpret_cast<uintptr_t*>(kTimerVtable)[1] == 0x36558250)
        {
            const struct
            {
                int slot;
                const void* hook;
            } timerHooks[] = {
                {1, reinterpret_cast<const void*>(&TimerUpdateOnFrameStart)},
                {2, reinterpret_cast<const void*>(&TimerGetCurrTime)},
                {3, reinterpret_cast<const void*>(&TimerGetFrameStartTime)},
                {6, reinterpret_cast<const void*>(&TimerGetFrameTime)},
                {7, reinterpret_cast<const void*>(&TimerGetRealFrameTime)},
                {8, reinterpret_cast<const void*>(&TimerGetTimeScale)},
                {12, reinterpret_cast<const void*>(&TimerGetFrameRate)},
            };
            for (const auto& h : timerHooks)
                PatchVtableSlot(kTimerVtable, h.slot, &g_timerOrig[h.slot], h.hook);
            SetupLog("render-thread frame time installed");
        }
    }
    if (g_mode == 2 && n->Length == 16 * 2 && _wcsnicmp(n->Buffer, L"CryAnimation.dll", 16) == 0 &&
        reinterpret_cast<uintptr_t>(data->base) == 0x31500000)
        PatchSkinRelease();
    if ((g_mode == 2 || g_censusOn) && n->Length == 15 * 2 && _wcsnicmp(n->Buffer, L"Cry3DEngine.dll", 15) == 0)
    {
        // r_DisplayInfo names the API from a literal used only there: show the back end in use.
        if (D3DLayerPacketsAvailable())
            PatchText(kDisplayApiName, "DX10", "DX12");
        if (g_mode == 2)
        {
            PatchVtableSlot(k3DEngineVtable, 7, reinterpret_cast<uintptr_t*>(&g_origLoadLevel), reinterpret_cast<const void*>(&HookLoadLevel));
            PatchVtableSlot(k3DEngineVtable, 9, reinterpret_cast<uintptr_t*>(&g_origUnloadLevel), reinterpret_cast<const void*>(&HookUnloadLevel));
            PatchVtableSlot(k3DEngineVtable, 10, reinterpret_cast<uintptr_t*>(&g_origPostLoadLevel), reinterpret_cast<const void*>(&HookPostLoadLevel));
        }
        PatchRenderSceneVtable(reinterpret_cast<uintptr_t>(data->base));
        // RenderWorld's inner render (FUN_3010AAC0) starts with CPartManager::Update (FUN_3007CFA0), which releases
        // dead emitters while the flush builds their vertices (crashed on a null container parameter block in
        // fill mode); clouds and volume objects are only moved there. Particle destruction is deferred.
        PatchParticleDelete();
        if (g_fillOn && !RedirectCall(kRenderInnerCall, kRenderInnerFn, reinterpret_cast<const void*>(&RenderInnerHook)))
            SetupLog("fill window: render inner call not hooked");
        SetupLog(PatchMaterialRefCount() ? "material reference counts made atomic" : "material reference count code differs, not patched");
        if (g_fillOn && GetEnvironmentVariableW(L"CRYMT_TERRAINOWN", nullptr, 0) &&
            (!RedirectCall(kTerrainReuseCall, kTerrainUpdateRenderMeshFn, reinterpret_cast<const void*>(&TerrainReuseHook)) ||
             !RedirectCall(kTerrainSurfaceCall, kTerrainSurfaceUpdateFn, reinterpret_cast<const void*>(&SurfaceUpdateHook))))
            SetupLog("terrain ownership: sector update calls not hooked");
        wchar_t terrainTex[8];
        g_terrainTexQueue = g_fillOn && GetEnvironmentVariableW(L"CRYMT_TERRAINTEX", terrainTex, 8) && wcscmp(terrainTex, L"0") != 0;
        if (g_terrainTexQueue && !RedirectCall(kTerrainTexturesCall, kTerrainTexturesFn, reinterpret_cast<const void*>(&TerrainTexturesHook)))
        {
            g_terrainTexQueue = false;
            SetupLog("fill window: terrain texture call not hooked");
        }
        // Terrain: CTerrain::CheckVis (0x300A7890) and UpdateNodesIncrementaly (0x300A8DC0, right after it) change
        // engine-side node data; what the flush draws (sector meshes, textures) changes through renderer calls, which
        // are sync points, except the sector texture ids (TerrainTexturesHook), and sectors are drawn into shadow maps
        // while main is held. Fence on request.
        if (g_fillOn && GetEnvironmentVariableW(L"CRYMT_TERRAINFENCE", nullptr, 0) &&
            !RedirectCall(0x3010E9C8, 0x300A7890, MakeFenceThunk(0x300A7890, 433)))
            SetupLog("fill window: terrain fence not installed");
        if (g_fillOn && GetEnvironmentVariableW(L"CRYMT_COMPILEFENCE", terrainTex, 8) && wcscmp(terrainTex, L"0") != 0 &&
            (!RedirectCall(kVoxelCompileCall, kVoxelCompileFn, reinterpret_cast<const void*>(&VoxelCompileHook)) ||
             !RedirectCall(kRoadCompileCall, kRoadCompileFn, reinterpret_cast<const void*>(&RoadCompileHook))))
            SetupLog("fill window: voxel/road compile calls not hooked");
        // Experiments: CRYMT_FENCEAT=site:target[,site:target...] (hex) makes main wait for the render thread before
        // those direct calls (reasons 440, 441, ...), to find which part of the fill window races with the flush.
        wchar_t fenceAt[256];
        if (g_fillOn && GetEnvironmentVariableW(L"CRYMT_FENCEAT", fenceAt, 256))
        {
            int reason = 440;
            for (wchar_t* p = fenceAt; *p;)
            {
                wchar_t* end = nullptr;
                const uintptr_t site = wcstoull(p, &end, 16);
                const uintptr_t target = *end == L':' ? wcstoull(end + 1, &end, 16) : 0;
                if (!target || !RedirectCall(site, target, MakeFenceThunk(target, reason++)))
                    SetupLog("fill window: a CRYMT_FENCEAT entry was not installed");
                p = *end == L',' ? end + 1 : end;
                if (!*end)
                    break;
            }
        }
        wchar_t mask[16] = {};
        int fences = GetEnvironmentVariableW(L"CRYMT_FENCE", mask, 16) ? _wtoi(mask) : 0;
        // Releasing main before shadow prep crashes on the first async frame (terrain LOD for shadow
        // sectors reads engine state main has already moved on), so it stays an experiment.
        g_shadowBarrier = GetEnvironmentVariableW(L"CRYMT_EARLYBARRIER", mask, 16) == 0;
        g_overlay.on = GetEnvironmentVariableW(L"CRYMT_OVERLAY", nullptr, 0) != 0; // Ctrl+F11 toggles it in game
        if (!g_shadowBarrier)
        {
            // Terrain work in C3DEngine::Update must not overlap shadow prep drawing terrain sectors.
            fences |= 16 | 32;
            RedirectCall(0x300F81B5, 0x30142AF0, MakeFenceThunk(0x30142AF0, 420));
            RedirectCall(0x300F831E, 0x300B6C00, MakeFenceThunk(0x300B6C00, 421));
        }
        if (fences & 1)
            FenceVtableEntry(0x301D3840, 0x300F5450, 401); // C3DEngine::OnFrameStart
        if (fences & 2)
            FenceVtableEntry(0x301D37F0, 0x300F7FB0, 402); // C3DEngine::Update
        if (fences & 4)
            FenceVtableEntry(0x301D3C78, 0x3001F0C0, 403); // C3DEngine::UpdateDeformableStatObj
        // Calls inside C3DEngine::Update, in order.
        const struct
        {
            int bit;
            uintptr_t site, target;
        } inner[] = {
            {8, 0x300F8132, 0x300E9C70},     // object manager update
            {16, 0x300F8143, 0x30141FE0},    // terrain
            {32, 0x300F814F, 0x301415C0},    // terrain
            {64, 0x300F8176, 0x300334B0},
            {128, 0x300F81BD, 0x3010DF20},
            {256, 0x300F8477, 0x3001E850},   // foliage list
        };
        for (const auto& c : inner)
            if (fences & c.bit)
                RedirectCall(c.site, c.target, MakeFenceThunk(c.target, 400 + c.bit));
    }
    if (n->Length == 8 * 2 && _wcsnicmp(n->Buffer, L"dxgi.dll", 8) == 0)
        PatchDxgi(reinterpret_cast<uintptr_t>(data->base));
    if (n->Length == 18 * 2 && _wcsnicmp(n->Buffer, L"CryRenderD3D10.dll", 18) == 0)
    {
        PatchDxgi(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"dxgi.dll")));
        TryPatchRenderer(reinterpret_cast<uintptr_t>(data->base));
        if (g_censusOn)
            InstallCensus();
        if (g_fillOn)
        {
            InstallFillHooks();
            if (!GetEnvironmentVariableW(L"CRYMT_NOPROXY", nullptr, 0))
                InstallProxyHooks();
            if (!RedirectCall(kMatricesDirtyJump, kMatricesDirtyFn, reinterpret_cast<const void*>(&MatricesDirtyHook)))
                SetupLog("fill window: matrices-dirty jump not patched");
        }
        if (g_dlssOn)
            InstallDlssHooks();
        else if (g_passNames && !InstallGpuProf())
            SetupLog("pass names: render list hook not installed");
        zpass::Install();
        if (g_gpuProf)
        {
            g_gpuProf = InstallGpuProf();
            int inner = 0;
            for (int i = 0; g_gpuProf && i < static_cast<int>(std::size(kGpuInnerSites)); i++)
                inner += RedirectCall(kGpuInnerSites[i].site, kGpuInnerSites[i].target,
                                      MakeHookThunk(kGpuInnerSites[i].target, &GpuInnerHook, i));
            char line[64];
            sprintf_s(line, "gpu pass timer installed=%d endframe marks=%d", g_gpuProf, inner);
            SetupLog(line);
        }
        WriteStats();
    }
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        g_faultPath = CryMTLog("crymt_fault.txt");
        g_hangPath = CryMTLog("crymt_hang.txt");
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        g_qpcToMs = 1000.0 / freq.QuadPart;
        wchar_t mode[8] = {};
        if (GetEnvironmentVariableW(L"CRYMT_MODE", mode, 8))
            g_mode = _wtoi(mode);
        // c1-launcher v8 carries the fix for the 64-bit allocator (pointers above 4 GB) and exports the memory
        // functions the deferred free needs; without it the render thread is not safe.
        if (g_mode == 2 && !GetProcAddress(GetModuleHandleW(nullptr), "CryMalloc"))
            TurnOff("CryMT off: Bin64\\Crysis.exe is not c1-launcher v8 (needed for the 64-bit memory fix)");
        if (g_off)
            g_mode = 0;
        else
            DeleteFileA(CryMTLog("crymt_off.txt").c_str());
        wchar_t trace[16] = {};
        if (GetEnvironmentVariableW(L"CRYMT_TRACE_FRAME", trace, 16))
            g_traceFrame = _wtoi(trace);
        wchar_t level[8] = {};
        if (GetEnvironmentVariableW(L"CRYMT_LEVEL", level, 8))
            g_level = _wtoi(level);
        SetPolicies();
        if (g_mode == 2 && !GetEnvironmentVariableW(L"CRYMT_NODEFERFREE", level, 8))
            InstallDeferredFree();
        g_sceneSync = GetEnvironmentVariableW(L"CRYMT_SCENESYNC", level, 8) != 0;
        g_noPump = GetEnvironmentVariableW(L"CRYMT_NOPUMP", level, 8) != 0;
        g_worldSync = GetEnvironmentVariableW(L"CRYMT_WORLDSYNC", level, 8) != 0;
        g_gpuProf = g_mode == 2 && GetEnvironmentVariableW(L"CRYMT_GPUPROF", level, 8) != 0;
        g_passNames = g_mode == 2 && (rdoc::Requested() || GetEnvironmentVariableW(L"CRYMT_MARKERS", level, 8) != 0);
        g_dlssOn = g_mode == 2 && (GetEnvironmentVariableW(L"CRYMT_DLSS", level, 8) != 0 || GetEnvironmentVariableW(L"CRYMT_GI", level, 8) != 0); // scene hand-over (DLSS, GI)
        g_pauseOnFocus = g_mode == 2 && GetEnvironmentVariableW(L"CRYMT_PAUSEONFOCUS", level, 8) != 0 && wcscmp(level, L"0") != 0;
        g_giKeys = GetEnvironmentVariableW(L"CRYMT_GI_KEYS", level, 8) != 0 && wcscmp(level, L"0") != 0;
        if (GetEnvironmentVariableA("CRYMT_RECORDKEYS", g_recordName, sizeof(g_recordName)) && g_recordName[0])
        {
            g_recordAuto = _stricmp(g_recordName, "auto") == 0;
            if (!g_recordAuto)
                g_giKeys = false; // the numpad records
        }
        g_giControl.active = g_giKeys;
        if (GetEnvironmentVariableW(L"CRYMT_GI_STARTTIME", level, 8))
            g_giStartTime = static_cast<float>(_wtof(level));
        if (g_giStartTime >= 0)
            g_giControl.startTime = g_giStartTime;
        if (GetEnvironmentVariableW(L"CRYMT_GI_TIMESPEED", level, 8))
            g_giTimeSpeed = static_cast<float>(_wtof(level));
        if (GetEnvironmentVariableA("CRYMT_TESTCMD", g_testCommands, sizeof(g_testCommands)))
            g_testInterval = GetEnvironmentVariableW(L"CRYMT_TESTCMDSEC", level, 8) ? _wtoi(level) : 60;
        g_frameLogOn = GetEnvironmentVariableW(L"CRYMT_FRAMELOG", level, 8) != 0 || g_gpuProf;
        g_censusOn = g_mode == 0 && GetEnvironmentVariableW(L"CRYMT_CENSUS", level, 8) != 0;
        g_fillOn = g_mode == 2 && GetEnvironmentVariableW(L"CRYMT_FILL", level, 8) != 0 && wcscmp(level, L"0") != 0; // 0: off
        g_frustumTest = g_mode == 2 && GetEnvironmentVariableW(L"CRYMT_FRUSTUMTEST", level, 8) != 0;
        if (g_fillOn)
            for (int s : {static_cast<int>(kSetDeferredTextures), 193}) // 193: 0x380E5860 stores a pointer at +0x28FA0
                g_policy[s] = kDefer;
        if (GetEnvironmentVariableW(L"CRYMT_BUFFERS", level, 8))
            g_backBuffers = static_cast<UINT>(std::min(_wtoi(level), 3));
        if (GetEnvironmentVariableW(L"CRYMT_SYNC_BEGINFRAME", level, 8))
            g_policy[kBeginFrame] = kSync;
        if (GetEnvironmentVariableW(L"CRYMT_WARMUP", level, 8))
            g_warmupFrames = _wtoi(level);
        AddVectoredExceptionHandler(1, OnException);
        if (g_mode == 2)
            CreateThread(nullptr, 0, WatchdogMain, nullptr, 0, nullptr);
        if (g_mode == 2 || g_off)
            CreateThread(nullptr, 0, TitleMain, nullptr, 0, nullptr);
        if (GetEnvironmentVariableW(L"CRYMT_HEARTBEAT", trace, 16))
            CreateThread(nullptr, 0, [](void*) -> DWORD {
                for (;;)
                {
                    Sleep(1000);
                    WriteStats();
                }
            }, nullptr, 0, nullptr);
        char line[48];
        sprintf_s(line, "mode=%d level=%d", g_mode, g_level);
        SetupLog(line);
        using Reg = LONG(NTAPI*)(ULONG, void*, void*, void**);
        auto reg = reinterpret_cast<Reg>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
        void* cookie = nullptr;
        if (reg)
            reg(0, reinterpret_cast<void*>(&OnDll), nullptr, &cookie);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        WriteStats();
        if (g_frameLogOn)
            WriteFrameLog();
    }
    return TRUE;
}
