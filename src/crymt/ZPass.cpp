// The renderer's z-prepass (FUN_380A5470), replaced piece by piece. Step 1, the census (CRYMT_ZCENSUS=1): while the
// original pass runs on the render thread, every call of the shader parameter setter (FUN_3803E670) is counted by
// shader, stage, constant buffer slot, register and parameter type, so the replacement knows which of the ~0x70
// parameter types it has to compute. The trace (CRYMT_ZTRACE=n, with CRYMT_D3DSTREAM): the n-th z-prepass's recorded
// D3D commands interleaved with marks for batch flushes, object changes, the per-object loops and each parameter set.
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Crysis6156.h"
#include "DrawPacket.h"
#include "ZPass.h"
#include "Paths.h"

void* DetourHead(uintptr_t fn, const uint8_t* expect, size_t len, const void* hook);
uint64_t CryMTWorldFrame(); // CryMT.cpp: world frames since start
int CryMTRenderLevel();      // CryMT.cpp: the 3D engine's render recursion level of the calling thread
bool D3DLayerTrace(std::vector<uint32_t>* sink);
const char* D3DLayerOpName(uint32_t op);
bool D3DLayerPacketsAvailable();
void D3DLayerDrawPacket(const uint8_t* packet, uint32_t bytes, void* const* objects, int count);
void Dx12DrawPacket(const uint8_t* packet);

// CryMT.cpp: pass names for frame debuggers (RenderDoc, PIX).
void PassNameBegin(uintptr_t renderer, uint32_t filter, const char* tag);
void PassNameEnd();

namespace zpass
{
namespace
{
using namespace ce2;

using ZPrepassFn = void(__fastcall*)(uintptr_t renderer);
using SetParamsFn = void*(__fastcall*)(uintptr_t params, int count, void* dst, int stage, uint32_t slot);
using NameFn = const char*(__fastcall*)(void*, int type);

using ObjectChangeFn = uint64_t(__fastcall*)(uintptr_t renderer, uintptr_t shader, uintptr_t res, uintptr_t object, uintptr_t re);
using FlushFn = void(__fastcall*)();
using DrawObjectsFn = void(__fastcall*)(uintptr_t renderer, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
using DrawObjectsAltFn = void(__fastcall*)(uintptr_t renderer, uintptr_t a, uintptr_t b);

ZPrepassFn g_zPrepass;
SetParamsFn g_setParams;
ObjectChangeFn g_objectChange;
FlushFn g_flush;
DrawObjectsFn g_drawObjects;
DrawObjectsAltFn g_drawObjectsAlt;
DWORD g_zThread; // the thread inside the original z-prepass, 0 outside it

// Trace marks: bit 31 set, kind in bits 24..30, payload below.
enum : uint32_t
{
    kMarkFlush = 1,   // payload: index into g_traceNames (the batch's shader)
    kMarkObject = 2,  // payload: object number within the pass
    kMarkDraw = 3,    // FUN_380268D0 entered
    kMarkDrawAlt = 4, // FUN_38025690 entered
    kMarkSetCall = 5, // payload: count | slot << 8 | dst << 16
    kMarkParam = 6,   // payload: type byte | stage << 8 | register << 12
};
// Cycle accounting (CRYMT_ZCYCLES=1): inclusive rdtsc cycles and calls of the hooked functions, on the render thread.
enum CycleSlot
{
    kCyZPrepass,
    kCyFlush,
    kCyObjects,
    kCyMeshDraw,
    kCySetGroup,
    kCyShaderSet,
    kCyMeshUpdate,
    kCyStreams,
    kCyPassFlags,
    kCyDeclA,
    kCyDeclB,
    kCyTechnique,
    kCyCommit,
    kCyInstLookup,
    kCyInstActivate,
    kCyShaderBind,
    kCyPsResources,
    kCySetGroup0,
    kCyOurItem,
    kCyGroupBase, // DrawGroup: the engine's draw of the first object
    kCyGroupOwn,  // DrawGroup: our part on the render thread (checks, capture, the stream command), with kCyGroupRest
    kCyGroupRest, // DrawGroup: the engine's draws of what we do not take (rejected groups, the rest of cut groups)
    kCySlots
};
const char* const kCycleNames[kCySlots] = {"z-prepass FUN_380A5470",      "batch flush FUN_3802D6D0", "per-object loop FUN_380268D0",
                                           "CREMesh draw vf15",           "per-instance group vf12(1)", "shader set CHWShader vf7",
                                           "mesh update CREMesh vf19",    "mesh streams CREMesh vf17", "pass flags FUN_3801BA60",
                                           "FUN_3801CCA0",                "FUN_3801CB80",             "technique FUN_3816EE70",
                                           "constant commit FUN_380452F0", "instance lookup FUN_380521F0", "instance activate FUN_3805AEA0",
                                           "shader bind FUN_3805ADA0",    "PS resources FUN_380477B0", "per-batch group vf12(0)",
                                           "own z-prepass item", "group base (engine)", "group own part (incl. rest)", "group rest (engine)"};
bool g_cycles;
uint64_t g_cyTicks[kCySlots], g_cyCalls[kCySlots];
uint64_t g_tscStart, g_qpcStart; // rdtsc rate from the time between Install and WriteStats

struct CycleScope
{
    int slot;
    uint64_t t0;
    explicit CycleScope(int s) : slot(g_cycles ? s : -1), t0(g_cycles ? __rdtsc() : 0) {}
    ~CycleScope()
    {
        if (slot >= 0)
        {
            g_cyTicks[slot] += __rdtsc() - t0;
            g_cyCalls[slot]++;
        }
    }
};

// Timing-only detours for functions taking up to eight integer or pointer arguments (no floats).
using AnyFn = uint64_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
template <int Slot>
struct Timed
{
    static inline AnyFn orig;
    static uint64_t __fastcall Hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
    {
        CycleScope scope(Slot);
        return orig(a, b, c, d, e, f, g, h);
    }
};

struct TimedSite
{
    uintptr_t fn;
    uint8_t head[10];
    size_t len;
};
constexpr TimedSite kTimedSites[] = {
    {0x3805B870, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18}, 10}, // shader set
    {0x381266F0, {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54}, 9},       // mesh update
    {0x380914C0, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x41, 0x57}, 8},             // mesh streams
    {0x3801BA60, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10}, 10}, // pass flags
    {0x3801CCA0, {0x40, 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00}, 9},
    {0x3801CB80, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30}, 10},
    {0x3816EE70, {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20}, 10}, // technique
    {0x380452F0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18}, 10}, // constant commit
    {0x380521F0, {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x41, 0x54}, 8},             // instance lookup (7 arguments)
    {0x3805AEA0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10}, 10}, // instance activation
    {0x3805ADA0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20}, 10}, // shader bind
    {0x380477B0, {0x48, 0x81, 0xEC, 0xB8, 0x01, 0x00, 0x00}, 7}, // PS resources (vf7 with flag 8): sub rsp, 0x1B8
};

template <int I>
void InstallTimedOne(bool skipShaderSet)
{
    if (I == 0 && skipShaderSet) // vf7 carries the reuse hook, which keeps the same cycle slot
        return;
    Timed<kCyShaderSet + I>::orig = reinterpret_cast<AnyFn>(
        DetourHead(kTimedSites[I].fn, kTimedSites[I].head, kTimedSites[I].len, reinterpret_cast<const void*>(&Timed<kCyShaderSet + I>::Hook)));
}

template <int... I>
void InstallTimed(std::integer_sequence<int, I...>, bool skipShaderSet)
{
    (InstallTimedOne<I>(skipShaderSet), ...);
}

uint32_t g_traceTarget; // pass number to trace (1-based), 0: off
bool g_tracing;
std::vector<uint32_t> g_trace;
std::vector<std::string> g_traceNames;
uint32_t g_traceObjects;

void Mark(uint32_t kind, uint32_t payload)
{
    if (g_tracing && g_zThread == GetCurrentThreadId())
        g_trace.push_back(0x80000000u | kind << 24 | (payload & 0xFFFFFF));
}

struct ParamKey
{
    uintptr_t shader;
    int32_t stage, slot, reg, vectors;
    uint32_t type, flags;
    bool operator==(const ParamKey& o) const
    {
        return memcmp(this, &o, sizeof(*this)) == 0;
    }
};

struct ParamKeyHash
{
    size_t operator()(const ParamKey& k) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&k);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(k); i++)
            h = (h ^ p[i]) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

struct ParamCount
{
    uint64_t count;
    std::string shader, types; // names resolved when first seen (shaders may be gone at exit)
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<ParamKey, ParamCount, ParamKeyHash> g_params;
uint64_t g_passes, g_setCalls, g_setParamsTotal;
uint64_t g_slotCalls[3][64]; // stage, constant buffer size in vectors

std::string ShaderName(uintptr_t shader)
{
    if (!shader)
        return "(none)";
    const auto getName = (*reinterpret_cast<const char* (__fastcall* const* const*)(uintptr_t)>(shader))[0x28 / 8];
    const char* name = getName(shader);
    return name ? name : "?";
}

std::string TypeNames(uint32_t type)
{
    std::string s;
    for (int c = 0; c < 4; c++)
    {
        const int t = static_cast<int>(type >> (c * 8) & 0xFF);
        if (!t && c)
            continue;
        const char* n = reinterpret_cast<NameFn>(kParamNameFn)(nullptr, t);
        if (!s.empty())
            s += ",";
        s += n ? n : "?";
        if (!n)
            s += std::to_string(t);
    }
    return s;
}

bool g_census;

void WriteTrace()
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("zpass_trace.txt").c_str(), "w") != 0 || !f)
        return;
    static const char* const kStages[4] = {"VS", "PS", "GS", "?"};
    fprintf(f, "z-prepass %u: %zu entries, %u objects\n", g_traceTarget, g_trace.size(), g_traceObjects);
    std::string last;
    int repeat = 0;
    const auto flushLine = [&] {
        if (!last.empty())
            fprintf(f, repeat > 1 ? "%s  x%d\n" : "%s\n", last.c_str(), repeat);
    };
    for (const uint32_t e : g_trace)
    {
        char line[200];
        if (e & 0x80000000u)
        {
            const uint32_t kind = e >> 24 & 0x7F, p = e & 0xFFFFFF;
            switch (kind)
            {
            case kMarkFlush:
                snprintf(line, sizeof(line), "== flush %s", p < g_traceNames.size() ? g_traceNames[p].c_str() : "?");
                break;
            case kMarkObject:
                snprintf(line, sizeof(line), "-- object");
                break;
            case kMarkDraw:
                snprintf(line, sizeof(line), "   [objects loop]");
                break;
            case kMarkDrawAlt:
                snprintf(line, sizeof(line), "   [objects loop alt]");
                break;
            case kMarkSetCall:
                snprintf(line, sizeof(line), "   set %u params, cb %u vectors%s", p & 0xFF, p >> 8 & 0xFF, p >> 16 & 1 ? " -> memory" : "");
                break;
            case kMarkParam:
                snprintf(line, sizeof(line), "     %s float %u %s", kStages[p >> 8 & 3], p >> 12, reinterpret_cast<NameFn>(kParamNameFn)(nullptr, static_cast<int>(p & 0xFF)));
                break;
            default:
                snprintf(line, sizeof(line), "mark %u %u", kind, p);
            }
        }
        else
            snprintf(line, sizeof(line), "   %s (%u bytes)", D3DLayerOpName(e & 0xFFFF), e >> 16);
        if (last == line)
            repeat++;
        else
        {
            flushLine();
            last = line;
            repeat = 1;
        }
    }
    flushLine();
    fclose(f);
}

void __fastcall ZPrepassHook(uintptr_t renderer)
{
    CycleScope cycles(kCyZPrepass);
    g_zThread = GetCurrentThreadId();
    g_passes++;
    if (g_traceTarget && g_passes == g_traceTarget && D3DLayerTrace(&g_trace))
        g_tracing = true;
    g_zPrepass(renderer);
    if (g_tracing)
    {
        D3DLayerTrace(nullptr);
        g_tracing = false;
        WriteTrace();
    }
    g_zThread = 0;
}

uint64_t __fastcall ObjectChangeHook(uintptr_t renderer, uintptr_t shader, uintptr_t res, uintptr_t object, uintptr_t re)
{
    if (g_tracing)
        Mark(kMarkObject, g_traceObjects++);
    return g_objectChange(renderer, shader, res, object, re);
}

// Repeat census (with CRYMT_ZCYCLES): how often a batch flush has the same setup inputs as the previous one, and
// whether equal inputs always gave the same shader instances.
struct FlushKey
{
    uintptr_t shader, resources, reVtable;
    uint64_t rtMask;
    uint32_t technique, vertexFormat, list, afterWater, filter, ltMask, mdMask, mdvMask, objFlags;
    bool operator==(const FlushKey& o) const
    {
        return memcmp(this, &o, sizeof(*this)) == 0;
    }
};
FlushKey g_lastKey;
uintptr_t g_lastInst[3];
uint64_t g_flushes, g_sameShader, g_sameKey, g_sameKeyOtherInst, g_sameInst;

void __fastcall FlushHook()
{
    CycleScope cycles(kCyFlush);
    if (g_cycles)
    {
        const uintptr_t r = *reinterpret_cast<const uintptr_t*>(kRendererGlobal);
        FlushKey k;
        memset(&k, 0, sizeof(k));
        k.shader = *reinterpret_cast<const uintptr_t*>(r + kRdShader);
        k.technique = *reinterpret_cast<const uint32_t*>(r + kRdTechnique);
        k.resources = *reinterpret_cast<const uintptr_t*>(r + 0xAB0);
        const uintptr_t re = *reinterpret_cast<const uintptr_t*>(r + kRdRendElement);
        k.reVtable = re ? *reinterpret_cast<const uintptr_t*>(re) : 0;
        // Only a CREMesh has a render mesh at +0x48.
        const bool isMesh = k.reVtable == kCREMeshVtable;
        const uintptr_t mesh = isMesh ? *reinterpret_cast<const uintptr_t*>(re + 0x48) : 0;
        k.vertexFormat = mesh ? *reinterpret_cast<const uint32_t*>(mesh + 0xFC) : 0;
        k.list = *reinterpret_cast<const uint32_t*>(r + kRdBatchListId);
        k.afterWater = *reinterpret_cast<const uint32_t*>(r + 0xF0C);
        k.filter = *reinterpret_cast<const uint32_t*>(r + kRdBatchFilter);
        k.ltMask = *reinterpret_cast<const uint32_t*>(r + 0xB08);
        k.rtMask = *reinterpret_cast<const uint64_t*>(r + 0xB10);
        k.mdMask = *reinterpret_cast<const uint32_t*>(r + 0xB18);
        k.mdvMask = *reinterpret_cast<const uint32_t*>(r + 0xB1C);
        const auto* objects = *reinterpret_cast<const uintptr_t* const*>(r + kRdBatchObjects);
        const uintptr_t obj = objects && *reinterpret_cast<const uint32_t*>(r + kRdBatchObjects + 8) ? objects[0] : 0;
        k.objFlags = obj ? *reinterpret_cast<const uint32_t*>(obj + 0x10) : 0;
        const bool sameShader = k.shader == g_lastKey.shader, sameKey = k == g_lastKey;
        g_flush();
        const uintptr_t inst[3] = {*reinterpret_cast<const uintptr_t*>(kCurVsInstance), *reinterpret_cast<const uintptr_t*>(kCurPsInstance),
                                   *reinterpret_cast<const uintptr_t*>(kCurGsInstance)};
        const bool sameInst = memcmp(inst, g_lastInst, sizeof(inst)) == 0;
        g_flushes++;
        g_sameShader += sameShader;
        g_sameKey += sameKey;
        g_sameInst += sameShader && sameInst;
        g_sameKeyOtherInst += sameKey && !sameInst;
        g_lastKey = k;
        memcpy(g_lastInst, inst, sizeof(inst));
        return;
    }
    if (g_tracing && g_zThread == GetCurrentThreadId())
    {
        const uintptr_t renderer = *reinterpret_cast<const uintptr_t*>(kRendererGlobal);
        g_traceNames.push_back(ShaderName(*reinterpret_cast<const uintptr_t*>(renderer + kRdShader)));
        Mark(kMarkFlush, static_cast<uint32_t>(g_traceNames.size() - 1));
    }
    g_flush();
}

extern bool g_packets;
extern int g_inObjects;
void FlushAllHeld();

// What the original's last per-object loops drew with (for the own z-prepass runs): the pass, and how many loops ran.
uintptr_t g_lastPass, g_lastLoopArg; // the loop's pass and its second argument (CREMesh vf15 takes it)
uint32_t g_objectLoops, g_instancedLoops;
uint64_t g_loopObjects; // objects of the plain per-object loops since the last reset

// Per-object loop census (CRYMT_LOOPCENSUS=1): every FUN_380268D0 call of every pass, by recursion level and batch
// filter: loops, objects, and the objects a renderer-free path could draw (a static triangle-list CREMesh, not skinned,
// without objdata instance matrices), keyed by the per-instance parameter types of the pass's VS/PS/GS instances. It
// tells which parameter types such a path must compute itself, pass by pass.
bool g_loopCensus;
struct LoopCount
{
    uint64_t loops, objects, plain;
};
std::map<std::tuple<uint32_t, uint32_t, std::string>, LoopCount> g_loopStats;
std::unordered_map<uintptr_t, std::string> g_instTypes; // shader instance -> its per-instance parameter types

bool IsMeshRe(uintptr_t re);
uintptr_t Vt(uintptr_t object, int slot);

const std::string& InstanceTypes(uintptr_t shader)
{
    static const std::string kNone;
    const uintptr_t inst = shader ? *reinterpret_cast<const uintptr_t*>(shader + 0x70) : 0;
    if (!inst)
        return kNone;
    auto [it, fresh] = g_instTypes.try_emplace(inst);
    if (fresh)
    {
        const auto* range = *reinterpret_cast<const uintptr_t* const*>(inst + 0x38 + 8);
        const int count = range && range[1] > range[0] ? static_cast<int>((range[1] - range[0]) / kParamSize) : 0;
        for (int i = 0; i < count; i++)
        {
            const uintptr_t p = range[0] + static_cast<uintptr_t>(i) * kParamSize;
            if (!((*reinterpret_cast<const uint32_t*>(p + 8) & 8) || *reinterpret_cast<const int16_t*>(p + 0xC) >= 0))
                continue;
            it->second += (it->second.empty() ? "" : ",") + TypeNames(*reinterpret_cast<const uint32_t*>(p + 0x18));
        }
    }
    return it->second;
}

void CountLoop(uintptr_t r, uintptr_t pass)
{
    const auto* list = reinterpret_cast<const uint8_t*>(r + kRdBatchObjects);
    const auto* objects = *reinterpret_cast<const uintptr_t* const*>(list);
    const int count = *reinterpret_cast<const int*>(list + 8);
    const uintptr_t re = *reinterpret_cast<const uintptr_t*>(r + kRdRendElement);
    bool plainBatch = false;
    if (re && IsMeshRe(re))
    {
        const uintptr_t mesh = *reinterpret_cast<const uintptr_t*>(re + 0x48);
        plainBatch = mesh && *reinterpret_cast<const int*>(mesh + kMeshPrimitive) == 0 &&
                     !(*reinterpret_cast<const int*>(0x382E994C) && reinterpret_cast<uint64_t(__fastcall*)(uintptr_t)>(Vt(re, 8))(re) & 0xFF);
    }
    uint64_t plain = 0;
    for (int i = 0; plainBatch && objects && i < count; i++)
    {
        const uintptr_t obj = objects[i];
        const int16_t od = obj ? *reinterpret_cast<const int16_t*>(obj + 0xC) : -1;
        plain += obj && !(od >= 0 && *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(kObjDataArrayPtr) +
                                                                        static_cast<uintptr_t>(od) * 0x20 + 0x10));
    }
    std::string types = "VS:" + InstanceTypes(*reinterpret_cast<const uintptr_t*>(pass + 8)) + " PS:" +
                        InstanceTypes(*reinterpret_cast<const uintptr_t*>(pass + 0x10));
    if (const uintptr_t gs = *reinterpret_cast<const uintptr_t*>(pass + 0x18))
        types += " GS:" + InstanceTypes(gs);
    if (!plainBatch)
        types += " [not a plain mesh]";
    LoopCount& c = g_loopStats[{*reinterpret_cast<const uint32_t*>(kRecurseLevel), *reinterpret_cast<const uint32_t*>(r + kRdBatchFilter), types}];
    c.loops++;
    c.objects += count > 0 ? static_cast<uint64_t>(count) : 0;
    c.plain += plain;
}

bool g_objTexCensus; // CRYMT_OBJTEXCENSUS=1: per-object loops by level/list: does the pass's PS read object textures
std::map<std::pair<uint32_t, uint32_t>, std::array<uint64_t, 2>> g_objTexStats;
bool PsReadsObjectTextures(uintptr_t psShader, uintptr_t res);
bool MergesWith(uintptr_t base, uintptr_t obj, bool relaxTexture = false);

// Procedural vegetation census (CRYMT_PROCCENSUS=1): CTerrain::UpdateNodesIncrementaly (Cry3DEngine FUN_300A8DC0, once
// a frame on main from RenderScene) updates the sectors of list +0x200 (FUN_300BC8B0 each, FUN_300BA330 drops those past
// 127), drops the procedural objects of the farthest nodes of list +0x210 while the sector pool has fewer than 8 free
// (FUN_300BD6A0), then generates the nearest node lacking them (CheckUpdateProcObjects FUN_300BB130; every node while
// the flag at *0x3022B8D8+0x750 is set). The node's time splits into the candidate tests (surface weight FUN_300B8B40,
// height FUN_300AF190) and the object creation (pool FUN_300BA000, bounding box FUN_300D48A0, FUN_300D37F0, octree
// insertion FUN_3019EE20, outermost call only). Calls of FUN_300A8DC0 of 8 ms or more are listed with their QPC start, to
// be matched with the frame log.
// The flag is the engine's precache after a camera jump ("Preparing textures, procedural objects and shaders for position",
// distance from the int cvar at cvars+0xC8): on harbor's camera cut it generated 9-13 nodes in one frame (20-23 ms of a
// 54-63 ms frame), while one node costs 1.6 ms on average and 5 ms at most.
// Budget (CRYMT_PROCBUDGET=<ms>, default 4, 0 off): a frame generates its first node, then more only while the nodes of
// that frame took less than the budget; FUN_300BB130 returns 0 without side effects for a node it does not generate
// (done, or outside the area), so a skipped node reads as "nothing to do" and comes first again next frame (the list is
// sorted by distance each frame).
bool g_procCensus;
uint64_t g_prBudget, g_prFrameTicks, g_prSkipped, g_prSpread; // budget in rdtsc ticks; this frame's nodes; skipped calls, frames cut short
int g_prFrameNodes;
enum ProcSlot
{
    kPrUpdate,
    kPrSector,
    kPrSectorDrop,
    kPrProcDrop,
    kPrNode,
    kPrSurface,
    kPrHeight,
    kPrAlloc,
    kPrBox,
    kPrSetup,
    kPrInsert,
    kPrSlots,
    kPrFirstInNode = kPrSurface
};
const char* const kProcNames[kPrSlots] = {"UpdateNodesIncrementaly", "sector FUN_300BC8B0", "sector drop FUN_300BA330", "proc objects drop FUN_300BD6A0",
                                          "node (did work) FUN_300BB130", "surface weight FUN_300B8B40", "height FUN_300AF190", "object from pool FUN_300BA000",
                                          "bounding box FUN_300D48A0", "FUN_300D37F0", "octree insert FUN_3019EE20"};
uint64_t g_prTicks[kPrSlots], g_prCalls[kPrSlots], g_prNodeMax, g_prNodesIdle;
uint64_t g_prCur[kPrSlots], g_prCurCalls[kPrSlots]; // the current UpdateNodesIncrementaly call
struct ProcLong
{
    int64_t qpc;
    uint64_t ticks[kPrSlots], calls[kPrSlots];
};
ProcLong g_prLong[64];
int g_prLongCount, g_prDepth, g_prInsertDepth;
bool g_prInUpdate;
using ProcNodeFn = char(__fastcall*)(uintptr_t node);
using ProcSurfaceFn = float(__fastcall*)(uintptr_t node, const float* pos, uint8_t surface);
using ProcHeightFn = float(__fastcall*)(uintptr_t terrain, float x, float y, int flags);
using Proc2Fn = uint64_t(__fastcall*)(uintptr_t a, uintptr_t b);
using ProcInsertFn = uint64_t(__fastcall*)(uintptr_t octree, uintptr_t obj, uintptr_t box, float radius, uintptr_t center);
ProcNodeFn g_prNode;
ProcSurfaceFn g_prSurface;
ProcHeightFn g_prHeight;
ProcInsertFn g_prInsert;

void ProcAdd(int slot, uint64_t dt)
{
    if (slot >= kPrFirstInNode && !g_prDepth)
        return;
    g_prTicks[slot] += dt;
    g_prCalls[slot]++;
    if (g_prInUpdate)
        g_prCur[slot] += dt, g_prCurCalls[slot]++;
}

// Functions of one or two integer arguments (one-argument ones ignore rdx).
template <int S>
struct ProcTimed
{
    static inline Proc2Fn orig;
    static uint64_t __fastcall Hook(uintptr_t a, uintptr_t b)
    {
        const uint64_t t0 = __rdtsc();
        const uint64_t r = orig(a, b);
        ProcAdd(S, __rdtsc() - t0);
        return r;
    }
};

uint64_t __fastcall ProcUpdateHook(uintptr_t terrain, uintptr_t b)
{
    g_prFrameTicks = 0;
    g_prFrameNodes = 0;
    const uint64_t skipped = g_prSkipped;
    if (!g_procCensus)
    {
        const uint64_t r = ProcTimed<kPrUpdate>::orig(terrain, b);
        g_prSpread += g_prSkipped != skipped;
        return r;
    }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    memset(g_prCur, 0, sizeof(g_prCur));
    memset(g_prCurCalls, 0, sizeof(g_prCurCalls));
    g_prInUpdate = true;
    const uint64_t t0 = __rdtsc();
    const uint64_t r = ProcTimed<kPrUpdate>::orig(terrain, b);
    const uint64_t dt = __rdtsc() - t0;
    g_prInUpdate = false;
    g_prSpread += g_prSkipped != skipped;
    g_prTicks[kPrUpdate] += dt;
    g_prCalls[kPrUpdate]++;
    g_prCur[kPrUpdate] = dt;
    g_prCurCalls[kPrUpdate] = 1;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double tscPerMs = static_cast<double>(__rdtsc() - g_tscStart) / ((static_cast<uint64_t>(q.QuadPart) - g_qpcStart) * 1000.0 / freq.QuadPart);
    if (dt >= 8.0 * tscPerMs && g_prLongCount < static_cast<int>(std::size(g_prLong)))
    {
        ProcLong& l = g_prLong[g_prLongCount++];
        l.qpc = q.QuadPart;
        memcpy(l.ticks, g_prCur, sizeof(l.ticks));
        memcpy(l.calls, g_prCurCalls, sizeof(l.calls));
    }
    return r;
}
char __fastcall ProcNodeHook(uintptr_t node)
{
    if (g_prBudget && g_prFrameNodes && g_prFrameTicks >= g_prBudget)
    {
        g_prSkipped++;
        return 0;
    }
    const uint64_t t0 = __rdtsc();
    g_prDepth++;
    const char r = g_prNode(node);
    g_prDepth--;
    const uint64_t dt = __rdtsc() - t0;
    if (r)
    {
        g_prFrameNodes++;
        g_prFrameTicks += dt;
        ProcAdd(kPrNode, dt);
        g_prNodeMax = std::max(g_prNodeMax, dt);
    }
    else
        g_prNodesIdle++;
    return r;
}
float __fastcall ProcSurfaceHook(uintptr_t node, const float* pos, uint8_t surface)
{
    const uint64_t t0 = __rdtsc();
    const float r = g_prSurface(node, pos, surface);
    ProcAdd(kPrSurface, __rdtsc() - t0);
    return r;
}
float __fastcall ProcHeightHook(uintptr_t terrain, float x, float y, int flags)
{
    const uint64_t t0 = __rdtsc();
    const float r = g_prHeight(terrain, x, y, flags);
    ProcAdd(kPrHeight, __rdtsc() - t0);
    return r;
}
uint64_t __fastcall ProcInsertHook(uintptr_t octree, uintptr_t obj, uintptr_t box, float radius, uintptr_t center)
{
    const uint64_t t0 = __rdtsc();
    g_prInsertDepth++;
    const uint64_t r = g_prInsert(octree, obj, box, radius, center);
    if (--g_prInsertDepth == 0)
        ProcAdd(kPrInsert, __rdtsc() - t0);
    return r;
}

template <int S>
bool ProcDetour(uintptr_t fn, const uint8_t* head, size_t len, const void* hook = reinterpret_cast<const void*>(&ProcTimed<S>::Hook))
{
    ProcTimed<S>::orig = reinterpret_cast<Proc2Fn>(DetourHead(fn, head, len, hook));
    return ProcTimed<S>::orig != nullptr;
}

void InstallProcHooks()
{
    wchar_t text[16] = {};
    const double budgetMs = GetEnvironmentVariableW(L"CRYMT_PROCBUDGET", text, 16) ? _wtof(text) : 4.0;
    const bool census = GetEnvironmentVariableW(L"CRYMT_PROCCENSUS", nullptr, 0) != 0;
    if (budgetMs <= 0 && !census)
        return;
    LARGE_INTEGER freq, q0, q1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&q0);
    const uint64_t t0 = __rdtsc();
    do
        QueryPerformanceCounter(&q1);
    while (q1.QuadPart - q0.QuadPart < freq.QuadPart / 500); // 2 ms for the rdtsc rate
    const double tscPerMs = static_cast<double>(__rdtsc() - t0) * freq.QuadPart / (1000.0 * (q1.QuadPart - q0.QuadPart));
    static const uint8_t kUpdate[] = {0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00};
    static const uint8_t kSector[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
    static const uint8_t kSaveRbxRbp[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10}; // FUN_300BA330, FUN_300BD6A0
    static const uint8_t kNode[] = {0x41, 0x55, 0x48, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00};
    static const uint8_t kSurface[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08};
    static const uint8_t kHeight[] = {0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00};
    static const uint8_t kAlloc[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50};
    static const uint8_t kBox[] = {0x48, 0x89, 0x5C, 0x24, 0x20};
    static const uint8_t kSetup[] = {0x81, 0x61, 0x0C, 0x93, 0x7F, 0xFF, 0xF8};
    static const uint8_t kInsert[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18};
    if (!ProcDetour<kPrUpdate>(0x300A8DC0, kUpdate, sizeof(kUpdate), reinterpret_cast<const void*>(&ProcUpdateHook)))
        return;
    g_prNode = reinterpret_cast<ProcNodeFn>(DetourHead(0x300BB130, kNode, sizeof(kNode), reinterpret_cast<const void*>(&ProcNodeHook)));
    if (!g_prNode)
        return;
    g_prBudget = budgetMs > 0 ? static_cast<uint64_t>(budgetMs * tscPerMs) : 0;
    if (!census)
        return;
    bool ok = ProcDetour<kPrSector>(0x300BC8B0, kSector, sizeof(kSector));
    ok &= ProcDetour<kPrSectorDrop>(0x300BA330, kSaveRbxRbp, sizeof(kSaveRbxRbp));
    ok &= ProcDetour<kPrProcDrop>(0x300BD6A0, kSaveRbxRbp, sizeof(kSaveRbxRbp));
    ok &= ProcDetour<kPrAlloc>(0x300BA000, kAlloc, sizeof(kAlloc));
    ok &= ProcDetour<kPrBox>(0x300D48A0, kBox, sizeof(kBox));
    ok &= ProcDetour<kPrSetup>(0x300D37F0, kSetup, sizeof(kSetup));
    g_prSurface = reinterpret_cast<ProcSurfaceFn>(DetourHead(0x300B8B40, kSurface, sizeof(kSurface), reinterpret_cast<const void*>(&ProcSurfaceHook)));
    g_prHeight = reinterpret_cast<ProcHeightFn>(DetourHead(0x300AF190, kHeight, sizeof(kHeight), reinterpret_cast<const void*>(&ProcHeightHook)));
    g_prInsert = reinterpret_cast<ProcInsertFn>(DetourHead(0x3019EE20, kInsert, sizeof(kInsert), reinterpret_cast<const void*>(&ProcInsertHook)));
    g_procCensus = ok && g_prSurface && g_prHeight && g_prInsert;
}

void WriteProcCensus(FILE* f)
{
    if (!g_procCensus)
        return;
    LARGE_INTEGER q, freq;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&freq);
    const double tscPerMs = static_cast<double>(__rdtsc() - g_tscStart) / ((static_cast<uint64_t>(q.QuadPart) - g_qpcStart) * 1000.0 / freq.QuadPart);
    const double frames = g_prCalls[kPrUpdate] ? static_cast<double>(g_prCalls[kPrUpdate]) : 1.0;
    fprintf(f, "terrain incremental update (main thread; %.0f cycles per ms): frames %llu; nodes generated %llu, idle node calls %llu, longest node %.1f ms\n",
            tscPerMs, static_cast<unsigned long long>(g_prCalls[kPrUpdate]), static_cast<unsigned long long>(g_prCalls[kPrNode]),
            static_cast<unsigned long long>(g_prNodesIdle), g_prNodeMax / tscPerMs);
    for (int i = 0; i < kPrFirstInNode; i++)
        fprintf(f, "  %-32s calls %8.2f per frame, %7.3f ms per frame, %7.3f ms per call\n", kProcNames[i], g_prCalls[i] / frames,
                g_prTicks[i] / tscPerMs / frames, g_prCalls[i] ? g_prTicks[i] / tscPerMs / g_prCalls[i] : 0.0);
    const double nodes = g_prCalls[kPrNode] ? static_cast<double>(g_prCalls[kPrNode]) : 1.0;
    fprintf(f, "  per generated node: %.2f ms\n", g_prTicks[kPrNode] / tscPerMs / nodes);
    for (int i = kPrFirstInNode; i < kPrSlots; i++)
        fprintf(f, "    %-30s calls %10.1f per node, %8.0f cycles per call, %7.2f ms per node\n", kProcNames[i], g_prCalls[i] / nodes,
                g_prCalls[i] ? static_cast<double>(g_prTicks[i]) / g_prCalls[i] : 0.0, g_prTicks[i] / tscPerMs / nodes);
    fprintf(f, "  budget %.1f ms: frames cut short %llu, node calls skipped %llu\n", g_prBudget / tscPerMs,
            static_cast<unsigned long long>(g_prSpread), static_cast<unsigned long long>(g_prSkipped));
    fprintf(f, "  calls of 8 ms or more (qpc start; ms and calls: update, sector, sector drop, proc drop, nodes):\n");
    for (int i = 0; i < g_prLongCount; i++)
    {
        const ProcLong& l = g_prLong[i];
        fprintf(f, "    %lld  %6.1f  %5.1f/%llu  %5.1f/%llu  %5.1f/%llu  %5.1f/%llu\n", static_cast<long long>(l.qpc), l.ticks[kPrUpdate] / tscPerMs,
                l.ticks[kPrSector] / tscPerMs, static_cast<unsigned long long>(l.calls[kPrSector]), l.ticks[kPrSectorDrop] / tscPerMs,
                static_cast<unsigned long long>(l.calls[kPrSectorDrop]), l.ticks[kPrProcDrop] / tscPerMs, static_cast<unsigned long long>(l.calls[kPrProcDrop]),
                l.ticks[kPrNode] / tscPerMs, static_cast<unsigned long long>(l.calls[kPrNode]));
    }
}

// Merge census (CRYMT_MERGECENSUS=1): every FUN_380A11F0 call of every pass where the render element is the batch's
// CREMesh, by recursion level, list and batch filter: merged, or the first CRenderObject field that differed.
using MergeFn = char(__fastcall*)(uintptr_t renderer, uintptr_t obj, uintptr_t prev, uintptr_t re);
MergeFn g_merge;
bool g_mergeCensus;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::array<uint64_t, 8>> g_mergeStats; // [0] merged, [1..7] by field
constexpr uint8_t kMergeHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10};

// Relaxed batches of the general list (level 1, list 2): the engine's batches joined across merge failures caused only by
// +0xCC/+0xCE. A sequence of merge tests continues while prev is the last tested object or the engine batch's first.
// Per relaxed batch: objects, and distinct +0xCC dwords (texture runs a per-texture loop split would need).
std::vector<uint32_t> g_rbTex;
uintptr_t g_rbLast, g_rbBase;
uint64_t g_rbBatches, g_rbObjects, g_rbDistinct, g_rbAdjacent, g_rbHist[6], g_rbDistinctHist[6];
void CloseRelaxedBatch()
{
    const size_t n = g_rbTex.size();
    if (!n)
        return;
    std::vector<uint32_t> sorted = g_rbTex;
    std::sort(sorted.begin(), sorted.end());
    const size_t distinct = static_cast<size_t>(std::unique(sorted.begin(), sorted.end()) - sorted.begin());
    size_t runs = 1;
    for (size_t i = 1; i < n; i++)
        runs += g_rbTex[i] != g_rbTex[i - 1];
    const auto bucket = [](size_t v) { return v <= 1 ? 0 : v == 2 ? 1 : v <= 4 ? 2 : v <= 8 ? 3 : v <= 16 ? 4 : 5; };
    g_rbBatches++;
    g_rbObjects += n;
    g_rbDistinct += distinct;
    g_rbAdjacent += runs;
    g_rbHist[bucket(n)]++;
    g_rbDistinctHist[bucket(distinct)]++;
    g_rbTex.clear();
}
// outcome: 0 merged, 1 failed only on +0xCC/+0xCE, 2 failed on another field.
void NoteRelaxedBatch(uintptr_t obj, uintptr_t prev, int outcome)
{
    const auto tex = [](uintptr_t o) { return *reinterpret_cast<const uint32_t*>(o + 0xCC); };
    if (prev != g_rbLast && prev != g_rbBase)
    {
        CloseRelaxedBatch();
        g_rbBase = prev;
        g_rbTex.push_back(tex(prev));
    }
    if (outcome == 2)
    {
        CloseRelaxedBatch();
        g_rbTex.push_back(tex(obj));
    }
    else
        g_rbTex.push_back(tex(obj));
    if (outcome)
        g_rbBase = obj;
    g_rbLast = obj;
}

bool TexRelaxRefuses(uintptr_t r, uintptr_t obj);
bool TexRelaxJoin(uintptr_t r, uintptr_t obj, uintptr_t prev, uintptr_t re);

char __fastcall MergeHook(uintptr_t r, uintptr_t obj, uintptr_t prev, uintptr_t re)
{
    if (TexRelaxRefuses(r, obj))
        return 0;
    const char merged = g_merge(r, obj, prev, re);
    if (g_mergeCensus && re && obj && prev && *reinterpret_cast<const uintptr_t*>(r + kRdRendElement) == re && *reinterpret_cast<const int*>(re + 8) == 9)
    {
        const auto u32 = [](uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); };
        const auto u64 = [](uintptr_t a) { return *reinterpret_cast<const uint64_t*>(a); };
        const int why = merged ? 0
                        : ((u32(obj + 0x10) ^ u32(prev + 0x10)) & 0x47FF0000) ? 1
                        : u32(obj + 0x8C) != u32(prev + 0x8C)                  ? 2
                        : u32(obj + 0x84) != u32(prev + 0x84)                  ? 3
                        : u32(obj + 0xCC) != u32(prev + 0xCC)                  ? 4
                        : u64(obj + 0xE8) != u64(prev + 0xE8)                  ? 5
                        : u64(obj + 0x110) != u64(prev + 0x110)                ? 6
                                                                              : 7;
        g_mergeStats[{*reinterpret_cast<const uint32_t*>(kRecurseLevel), *reinterpret_cast<const uint32_t*>(r + kRdBatchListId),
                      *reinterpret_cast<const uint32_t*>(r + kRdBatchFilter)}][why]++;
        if (*reinterpret_cast<const uint32_t*>(kRecurseLevel) == 1 && *reinterpret_cast<const uint32_t*>(r + kRdBatchListId) == 2)
            NoteRelaxedBatch(obj, prev, merged ? 0 : why == 4 && u64(obj + 0xE8) == u64(prev + 0xE8) && u64(obj + 0x110) == u64(prev + 0x110) ? 1 : 2);
    }
    return merged || TexRelaxJoin(r, obj, prev, re) ? 1 : 0;
}

void WriteMergeCensus(FILE* f)
{
    if (g_objTexCensus)
    {
        const double frames = g_passes ? static_cast<double>(g_passes) : 1.0;
        fprintf(f, "per-object loops per frame, PS without / with object textures (level list):\n");
        for (const auto& [k, v] : g_objTexStats)
            fprintf(f, "  %u %2u: %8.1f %8.1f\n", k.first, k.second, v[0] / frames, v[1] / frames);
    }
    if (!g_mergeCensus)
        return;
    const double frames = g_passes ? static_cast<double>(g_passes) : 1.0;
    fprintf(f, "merge tests with the batch's CREMesh, per frame (level list filter: merged | flags light-mask layers tex(+0xCC) shadow-casters char other):\n");
    for (const auto& [k, v] : g_mergeStats)
        fprintf(f, "  %u %2u %5x: %8.1f | %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), v[0] / frames,
                v[1] / frames, v[2] / frames, v[3] / frames, v[4] / frames, v[5] / frames, v[6] / frames, v[7] / frames);
    CloseRelaxedBatch();
    fprintf(f, "relaxed batches of the general list (merge failures on +0xCC/+0xCE ignored), per frame: batches %.1f, objects %.1f, "
               "distinct +0xCC values %.1f, adjacent texture runs %.1f\n",
            g_rbBatches / frames, g_rbObjects / frames, g_rbDistinct / frames, g_rbAdjacent / frames);
    fprintf(f, "  by objects (1, 2, 3-4, 5-8, 9-16, 17+): %.1f %.1f %.1f %.1f %.1f %.1f; by distinct values: %.1f %.1f %.1f %.1f %.1f %.1f\n",
            g_rbHist[0] / frames, g_rbHist[1] / frames, g_rbHist[2] / frames, g_rbHist[3] / frames, g_rbHist[4] / frames, g_rbHist[5] / frames,
            g_rbDistinctHist[0] / frames, g_rbDistinctHist[1] / frames, g_rbDistinctHist[2] / frames, g_rbDistinctHist[3] / frames,
            g_rbDistinctHist[4] / frames, g_rbDistinctHist[5] / frames);
}

void WriteLoopCensus(FILE* f)
{
    if (!g_loopCensus)
        return;
    std::vector<std::pair<std::tuple<uint32_t, uint32_t, std::string>, LoopCount>> rows(g_loopStats.begin(), g_loopStats.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.objects > b.second.objects; });
    uint64_t loops = 0, objects = 0, plain = 0;
    for (const auto& r : rows)
        loops += r.second.loops, objects += r.second.objects, plain += r.second.plain;
    const double frames = g_passes ? static_cast<double>(g_passes) : 1.0;
    fprintf(f, "per-object loops (per frame over %.0f frames): loops %.1f, objects %.1f, plain objects %.1f\n", frames, loops / frames,
            objects / frames, plain / frames);
    fprintf(f, "  level filter      loops    objects      plain  per-instance types\n");
    for (size_t i = 0; i < rows.size() && i < 60; i++)
        fprintf(f, "  %5u %6x %10.1f %10.1f %10.1f  %s\n", std::get<0>(rows[i].first), std::get<1>(rows[i].first), rows[i].second.loops / frames,
                rows[i].second.objects / frames, rows[i].second.plain / frames, std::get<2>(rows[i].first).c_str());
}

constexpr uintptr_t kRdFrameId = 0x291E8;
bool g_loopGroups, g_loopGroupVerify;
bool g_loopGroupsToggle; // CRYMT_LOOPGROUPS=toggle: only on odd frames, for same-run comparisons of the render thread's time
bool LoopGroup(uintptr_t r, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);

// Relaxed texture merge in the general pass (CRYMT_TEXRELAX=1 | toggle: odd frames only, for same-run comparisons).
// FUN_380A11F0 refuses to merge objects whose m_nRAEId/m_nTextureID dword (+0xCC) differs: ~1100 batches a frame on
// harbor, objects of one mesh alternating between a few textures. Its success path appends the object (and prev for an
// empty list) to the batch list r+0xAC0 (FUN_381A5D60), ORs flag 0x200 into r+0xAEC, keeps the least distance in r+0xAE8
// and, for objdata instance matrices, sets r+0xAFC bit 0x10000000 (the flush then draws through FUN_38025690, not the
// loop). Here such an object joins the batch the same way when it differs from prev only in +0xCC (MergesWith with
// relaxTexture), in EFSLIST_GENERAL, with r+0x13DEC 0, and +0xCE > 0 agrees with the batch's current object (the RT
// mask, FUN_38029090, reads it under flag 0x1000). Objects with objdata instance matrices do not merge into a mixed batch.
// The flush (FUN_3802D6D0) sets r+0xAFC bit 0x10000000 when it instances the batch (cvar 0x382E9A64, more objects than
// 0x382E9B64 or the bit already set, shader +0x3C & 0x6000000, 0x382E98F8 == 0); each pass then runs FUN_38025690(r, a,
// pass), which binds the PS resources itself (vf7(PS, 8) from the current object r+0xA78) and draws instances. Otherwise
// per pass the engine binds the PS resources once (vf7(PS, 8) -> FUN_380477B0, which reads +0xCC/+0xCE/+0xD0 of r+0xA78)
// and runs the loop FUN_380268D0(r, a, pass, VS, PS), which sets r+0xA78 to each object and restores it at the end.
// A mixed list is drawn once per texture run: r+0xA78 = the run's first object, then the instanced path on the run, or
// (loop path, when the PS reads object textures: PsReadsObjectTextures) vf7(PS, 8) when the run's texture differs from
// the bound one and the loop on the run. Runs are grouped by texture when the pass does not blend (r+0xB30 & 0xFF == 0:
// the state EF_SetState keeps), else kept in order. r+0xA78 is restored after, so the next pass binds as the engine would.
bool g_texRelax, g_texRelaxToggle, g_texRelaxEven, g_inSplit; // toggle: odd frames; toggle2: even frames
uint64_t g_trJoined, g_trNoObjdata, g_trRefused, g_trMixed, g_trSameTex, g_trRuns, g_trRebinds, g_trResFail, g_trAltMixed, g_trAltRuns;
using ShaderSetFlagsFn = char(__fastcall*)(uintptr_t shader, int flags);

bool TexRelaxOn(uintptr_t r)
{
    return g_texRelax && (!g_texRelaxToggle || (*reinterpret_cast<const uint32_t*>(r + kRdFrameId) & 1) != static_cast<uint32_t>(g_texRelaxEven));
}

bool ListMixed(uintptr_t r)
{
    const auto* objs = *reinterpret_cast<uintptr_t* const*>(r + kRdBatchObjects);
    const int count = *reinterpret_cast<const int*>(r + kRdBatchObjects + 8);
    for (int i = 1; i < count && objs; i++)
        if (*reinterpret_cast<const uint32_t*>(objs[i] + 0xCC) != *reinterpret_cast<const uint32_t*>(objs[0] + 0xCC))
            return true;
    return false;
}

bool HasObjdataMatrices(uintptr_t obj)
{
    const int16_t od = *reinterpret_cast<const int16_t*>(obj + 0xC);
    return od >= 0 && *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(kObjDataArrayPtr) + static_cast<uintptr_t>(od) * 0x20 + 0x10);
}

// Before the engine's test: an object with objdata instance matrices would send a mixed batch to FUN_38025690.
bool TexRelaxRefuses(uintptr_t r, uintptr_t obj)
{
    if (!g_texRelax || !obj || !HasObjdataMatrices(obj) || !ListMixed(r))
        return false;
    g_trNoObjdata++;
    return true;
}

bool TexRelaxJoin(uintptr_t r, uintptr_t obj, uintptr_t prev, uintptr_t re)
{
    if (!TexRelaxOn(r) || !obj || !prev || !re || *reinterpret_cast<const uintptr_t*>(r + kRdRendElement) != re || *reinterpret_cast<const int*>(re + 8) != 9 ||
        *reinterpret_cast<const uint32_t*>(r + kRdBatchListId) != 2 || *reinterpret_cast<const int*>(r + 0x13DEC) != 0 ||
        *reinterpret_cast<const uint32_t*>(obj + 0xCC) == *reinterpret_cast<const uint32_t*>(prev + 0xCC) || !MergesWith(prev, obj, true))
        return false;
    const uintptr_t cur = *reinterpret_cast<const uintptr_t*>(r + kRdCurObject);
    if ((*reinterpret_cast<const int16_t*>(obj + 0xCE) > 0) != (*reinterpret_cast<const int16_t*>((cur ? cur : prev) + 0xCE) > 0))
        return g_trRefused++, false;
    const auto append = reinterpret_cast<void(__fastcall*)(uintptr_t list, uintptr_t obj)>(0x381A5D60);
    if (*reinterpret_cast<const int*>(r + kRdBatchObjects + 8) == 0)
        append(r + kRdBatchObjects, prev);
    append(r + kRdBatchObjects, obj);
    *reinterpret_cast<uint32_t*>(r + 0xAEC) |= *reinterpret_cast<const uint32_t*>(obj + 0x10) & 0x200;
    float& least = *reinterpret_cast<float*>(r + 0xAE8);
    least = std::min(least, *reinterpret_cast<const float*>(obj + 0xE0));
    g_trJoined++;
    return true;
}

void __fastcall DrawObjectsHook(uintptr_t renderer, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
void __fastcall DrawObjectsAltHook(uintptr_t renderer, uintptr_t a, uintptr_t b);

// A mixed list once per texture run (alt: the instanced path FUN_38025690); false: the caller draws it as usual.
bool SplitByTexture(uintptr_t r, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, bool alt)
{
    if (!g_texRelax || g_inSplit || !ListMixed(r))
        return false;
    const uintptr_t ps = *reinterpret_cast<const uintptr_t*>(b + 0x10);
    if (alt)
        g_trAltMixed++;
    else
    {
        g_trMixed++;
        if (!ps || !PsReadsObjectTextures(ps, *reinterpret_cast<const uintptr_t*>(r + kRdShaderRes)))
            return g_trSameTex++, false;
    }
    auto* list = reinterpret_cast<uint8_t*>(r + kRdBatchObjects);
    uintptr_t* const objs = *reinterpret_cast<uintptr_t**>(list);
    const int count = *reinterpret_cast<int*>(list + 8);
    const uintptr_t cur = *reinterpret_cast<const uintptr_t*>(r + kRdCurObject);
    const auto key = [](uintptr_t o) { return *reinterpret_cast<const uint32_t*>(o + 0xCC); };
    uint32_t bound = key(cur ? cur : objs[0]);
    static std::vector<uintptr_t> order;
    order.assign(objs, objs + count);
    if ((*reinterpret_cast<const uint32_t*>(r + 0xB30) & 0xFF) == 0)
        std::stable_sort(order.begin(), order.end(), [&](uintptr_t x, uintptr_t y) {
            const uint32_t kx = key(x), ky = key(y);
            return (kx != bound) < (ky != bound) || ((kx != bound) == (ky != bound) && kx < ky);
        });
    const auto set = reinterpret_cast<ShaderSetFlagsFn>(Vt(ps, 7));
    g_inSplit = true;
    for (int s = 0; s < count;)
    {
        int e = s + 1;
        while (e < count && key(order[e]) == key(order[s]))
            e++;
        *reinterpret_cast<uintptr_t*>(r + kRdCurObject) = order[s];
        bool ok = true;
        if (alt)
            g_trAltRuns++;
        else if (key(order[s]) != bound)
        {
            ok = set(ps, 8) != 0;
            bound = key(order[s]);
            g_trRebinds++;
            g_trResFail += !ok;
        }
        if (ok)
        {
            *reinterpret_cast<uintptr_t**>(list) = order.data() + s;
            *reinterpret_cast<int*>(list + 8) = e - s;
            if (alt)
                DrawObjectsAltHook(r, a, b);
            else
                DrawObjectsHook(r, a, b, c, d);
        }
        g_trRuns++;
        s = e;
    }
    g_inSplit = false;
    *reinterpret_cast<uintptr_t**>(list) = objs;
    *reinterpret_cast<int*>(list + 8) = count;
    *reinterpret_cast<uintptr_t*>(r + kRdCurObject) = cur;
    return true;
}

void __fastcall DrawObjectsHook(uintptr_t renderer, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    if (SplitByTexture(renderer, a, b, c, d, false))
        return;
    CycleScope cycles(kCyObjects);
    if (g_loopCensus)
        CountLoop(renderer, b);
    g_lastPass = b;
    g_lastLoopArg = a;
    g_objectLoops++;
    g_loopObjects += static_cast<uint64_t>(std::max(0, *reinterpret_cast<const int*>(renderer + kRdBatchObjects + 8)));
    Mark(kMarkDraw, 0);
    if (g_loopGroups && (!g_loopGroupsToggle || (*reinterpret_cast<const uint32_t*>(renderer + kRdFrameId) & 1)) && LoopGroup(renderer, a, b, c, d))
        return;
    if (g_objTexCensus)
    {
        g_drawObjects(renderer, a, b, c, d);
        const bool reads = PsReadsObjectTextures(*reinterpret_cast<const uintptr_t*>(b + 0x10), *reinterpret_cast<const uintptr_t*>(renderer + kRdShaderRes));
        g_objTexStats[{*reinterpret_cast<const uint32_t*>(kRecurseLevel), *reinterpret_cast<const uint32_t*>(renderer + kRdBatchListId)}][reads]++;
        return;
    }
    if (!g_packets)
    {
        g_drawObjects(renderer, a, b, c, d);
        return;
    }
    // Per-instance groups are held for packets only inside the loop; whatever is held at its end is written.
    g_inObjects++;
    g_drawObjects(renderer, a, b, c, d);
    if (--g_inObjects == 0)
        FlushAllHeld();
}

void __fastcall DrawObjectsAltHook(uintptr_t renderer, uintptr_t a, uintptr_t b)
{
    if (SplitByTexture(renderer, a, b, 0, 0, true))
        return;
    g_instancedLoops++;
    Mark(kMarkDrawAlt, 0);
    g_drawObjectsAlt(renderer, a, b);
}

void* __fastcall SetParamsHook(uintptr_t params, int count, void* dst, int stage, uint32_t slot)
{
    if (g_tracing && g_zThread == GetCurrentThreadId() && params && count > 0)
    {
        Mark(kMarkSetCall, static_cast<uint32_t>(count & 0xFF) | (slot & 0xFF) << 8 | (dst ? 1u : 0u) << 16);
        for (int i = 0; i < count; i++)
        {
            const uintptr_t p = params + static_cast<uintptr_t>(i) * kParamSize;
            const uint32_t reg = static_cast<uint32_t>(*reinterpret_cast<const int16_t*>(p + 0xC)) & 0xFFF;
            Mark(kMarkParam, (*reinterpret_cast<const uint32_t*>(p + 0x18) & 0xFF) | static_cast<uint32_t>(stage & 3) << 8 | reg << 12);
        }
    }
    if (g_census && g_zThread && g_zThread == GetCurrentThreadId() && params && count > 0)
    {
        const uintptr_t renderer = *reinterpret_cast<const uintptr_t*>(kRendererGlobal);
        const uintptr_t shader = renderer ? *reinterpret_cast<const uintptr_t*>(renderer + kRdShader) : 0;
        AcquireSRWLockExclusive(&g_lock);
        g_setCalls++;
        g_setParamsTotal += static_cast<uint64_t>(count);
        if (stage >= 0 && stage < 3 && slot < 64)
            g_slotCalls[stage][slot]++;
        for (int i = 0; i < count; i++)
        {
            const uintptr_t p = params + static_cast<uintptr_t>(i) * kParamSize;
            ParamKey k;
            memset(&k, 0, sizeof(k));
            k.shader = shader;
            k.stage = stage;
            k.slot = static_cast<int32_t>(slot);
            k.reg = *reinterpret_cast<const int16_t*>(p + 0xC);
            k.vectors = *reinterpret_cast<const int32_t*>(p + 0x10);
            k.type = *reinterpret_cast<const uint32_t*>(p + 0x18);
            k.flags = *reinterpret_cast<const uint32_t*>(p + 8);
            auto [it, fresh] = g_params.try_emplace(k);
            if (fresh)
            {
                it->second.shader = ShaderName(shader);
                it->second.types = TypeNames(k.type);
            }
            it->second.count++;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return g_setParams(params, count, dst, stage, slot);
}
// ---------------------------------------------------------------------------------------------------------------------
// Per-instance shader parameters of our own (CRYMT_PIFAST=1): CHWShader_D3D vf12 for group 1 (called per object by
// the per-object loops of every pass) computes the values itself when every parameter of the group is a type we
// know, and writes them through the engine's constant buffer writer, so its buffer caches stay right; any other group
// runs the original. CRYMT_PIVERIFY=1 computes the same values, runs the original with its constant buffer writes
// captured, and compares them bit for bit.
// ---------------------------------------------------------------------------------------------------------------------
using SetGroupFn = void(__fastcall*)(uintptr_t shader, int group);
using CbWriteFn = void(__fastcall*)(int reg, int slot, int stage, const void* data, int floats, int cbVectors);

SetGroupFn g_setGroup;
CbWriteFn g_cbWrite = reinterpret_cast<CbWriteFn>(kCbWriteFn);
bool g_piFast, g_piVerify, g_typesResolved;
int g_tObjMatrix = -1;
int g_tBendInfo = -1, g_tAlphaTest = -1, g_tObjectAmbColComp = -1, g_tAmbientOpacity = -1;
uint64_t g_piCalls, g_piFast_, g_piVerified, g_piMismatches, g_piUnseen;
uint64_t g_piTicks, g_piTimed; // rdtsc cycles of the groups we handle: the original's (verify) or ours (fast)
std::unordered_map<int, uint64_t> g_piUnsupported; // first unsupported parameter type of a group -> calls
std::string g_mismatchLog;

constexpr int kMaxGroupParams = 8;
struct Expect
{
    int reg, slot, stage, floats;
    float v[16];
    bool seen;
};
Expect g_expect[kMaxGroupParams];
int g_expectCount;
bool g_capturing;

void ResolveTypes()
{
    if (g_typesResolved)
        return;
    g_typesResolved = true;
    for (int t = 0; t < 256; t++)
    {
        const char* n = reinterpret_cast<NameFn>(kParamNameFn)(nullptr, t);
        if (n && strcmp(n, "PI_ObjMatrix") == 0)
            g_tObjMatrix = t;
        if (n && strcmp(n, "PI_BendInfo") == 0)
            g_tBendInfo = t;
        if (n && strcmp(n, "PI_AlphaTest") == 0)
            g_tAlphaTest = t;
        if (n && strcmp(n, "PI_ObjectAmbColComp") == 0)
            g_tObjectAmbColComp = t;
        if (n && strcmp(n, "PI_AmbientOpacity") == 0)
            g_tAmbientOpacity = t;
    }
}

// Inputs of the per-instance values, so they can be computed away from the renderer: what the batch set up (renderer
// fields after the engine's flush) and the object. CapturePi takes them from the renderer as the original reads them.
struct PiShared
{
    uint64_t res, shader, entries;
    uint32_t flagsA, flagsB, b20; // renderer +0xAF4, +0xAEC (the batch's object flags), +0xB20
    float scale, origin[3];       // ambient scale +0x13E94, camera-relative origin +0x28F44
    uint8_t matFlag;              // the resources' flag byte FUN_38036D50 tests (bit 4: no fade)
    uint8_t nightCheck, nightKnown, nightActive;
    uint8_t pad[4];
};
struct PiObject
{
    uint64_t obj, matrix; // CRenderObject, its 3x4 matrix block (renderer +0xA80 while it is drawn)
};

using ResFlagFn = uint8_t(__fastcall*)(uintptr_t);
using ParamValueFn = float(__fastcall*)(uintptr_t param);
void CaptureShared(uintptr_t r, PiShared& s)
{
    const auto u32 = [r](uintptr_t off) { return *reinterpret_cast<const uint32_t*>(r + off); };
    s.res = *reinterpret_cast<const uintptr_t*>(r + kRdShaderRes);
    s.shader = *reinterpret_cast<const uintptr_t*>(r + kRdShader);
    s.entries = *reinterpret_cast<const uintptr_t*>(kObjectShaderEntries);
    s.flagsA = u32(kRdObjFlagsA);
    s.flagsB = u32(kRdObjFlagsB);
    s.b20 = u32(0xB20);
    s.scale = *reinterpret_cast<const float*>(r + 0x13E94);
    memcpy(s.origin, reinterpret_cast<const void*>(r + kRdRelativeOrigin), sizeof(s.origin));
    // FUN_38036D50's first call: the resources' secondary interface at +0x38, slot 12.
    s.matFlag = s.res ? reinterpret_cast<ResFlagFn>((*reinterpret_cast<const uintptr_t* const*>(s.res + 0x38))[0x60 / 8])(s.res + 0x38) : 0;
    s.nightCheck = s.res && *reinterpret_cast<const int*>(0x382E98B4) && *reinterpret_cast<const int*>(0x382E98C8);
    const uintptr_t param = *reinterpret_cast<const uintptr_t*>(0x382DAB08);
    s.nightKnown = s.nightCheck && (*reinterpret_cast<const uint32_t*>(0x382DAEA8) & 2) && param;
    s.nightActive = 0;
    if (s.nightKnown)
    {
        const float active = reinterpret_cast<ParamValueFn>((*reinterpret_cast<const uintptr_t* const*>(param))[0x38 / 8])(param);
        s.nightActive = active < 0.0f || active > 0.0f; // ucomiss/je: equal or unordered means off
    }
}

void CapturePi(PiShared& s, PiObject& o)
{
    const uintptr_t r = *reinterpret_cast<const uintptr_t*>(kRendererGlobal);
    CaptureShared(r, s);
    o.obj = *reinterpret_cast<const uintptr_t*>(r + kRdCurObject);
    o.matrix = *reinterpret_cast<const uintptr_t*>(r + kRdObjMatrix);
}

// The original's case for PI_ObjMatrix: the object's 3x4 matrix, camera-relative unless the flags say otherwise.
bool ObjMatrix(const PiShared& s, const PiObject& o, float* out)
{
    if (!o.matrix)
        return false;
    memcpy(out, reinterpret_cast<const void*>(o.matrix), 12 * sizeof(float));
    if (!(s.flagsA >> 20 & 1) && (s.flagsB & 7) && !(s.flagsB >> 10 & 1))
    {
        out[3] -= s.origin[0];
        out[7] -= s.origin[1];
        out[11] -= s.origin[2];
    }
    return true;
}

// PI_BendInfo, FUN_38038AD0 without its cache (keyed by the per-object draw counter renderer +0xB44, so a hit returns
// what the same object computed): vegetation bending from the object's +0xFC/+0x100/+0x128 and, when the object has
// shader entries (+0xD2 > 0), two wave forms of g_ObjectShaderEntries (they read the renderer's time, +0xAE4, set at
// each flush start). Same operations and order as the original, and its NaN behaviour (ucomiss/jne: unordered is equal).
using WaveFn = float(__fastcall*)(uintptr_t entry);
bool BendInfo(const PiShared& s, const PiObject& o, float* out)
{
    const uintptr_t obj = o.obj;
    const auto field = [obj](uintptr_t off) { return *reinterpret_cast<const float*>(obj + off); };
    const auto isZero = [](float v) { return !(v < 0.0f || v > 0.0f); };
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!obj)
        return true;
    const float fc = field(0xFC), f100 = field(0x100), f128 = field(0x128);
    if (!(fabsf(fc + f100 + f128) > 0.0f))
        return true;
    float c0, c1, c2;
    if (isZero(f128))
    {
        c0 = c1 = 0.0f;
        c2 = sqrtf(fc * fc + f100 * f100);
    }
    else
    {
        c0 = fc;
        c1 = f100;
        c2 = f128;
    }
    const int16_t entry = *reinterpret_cast<const int16_t*>(obj + 0xD2);
    if (entry > 0)
    {
        c0 = c0 + reinterpret_cast<WaveFn>(kWaveFn)(s.entries + static_cast<uintptr_t>(entry) * 0x14);
        c1 = c1 + reinterpret_cast<WaveFn>(kWaveFn)(s.entries + static_cast<uintptr_t>(entry + 1) * 0x14);
    }
    if (isZero(f128))
    {
        const float z = sqrtf(fc * fc + f100 * f100);
        out[0] = c0 * 50.0f;
        out[1] = c1 * 50.0f;
        out[2] = z;
        out[3] = z * 0.25f;
    }
    else
    {
        out[0] = c0;
        out[1] = c1;
        out[2] = c2;
        out[3] = sqrtf(c0 * c0 + c1 * c1);
    }
    return true;
}

// PI_AlphaTest (FUN_3803E670 case 0x49): w = the material's alpha reference (resources +0xC), 0.51 for shaders with flag
// bit 23 unless renderer +0xAF4 bit 20, plus the object's +0x8A byte / 255.
bool AlphaTest(const PiShared& s, const PiObject& o, float* out)
{
    if (!s.shader || !o.obj)
        return false;
    float w = s.res ? *reinterpret_cast<const float*>(s.res + 0xC) : 0.0f;
    if ((*reinterpret_cast<const uint32_t*>(s.shader + 0x40) >> 23 & 1) && !(s.flagsA >> 20 & 1))
        w = 0.51f;
    if (const uint8_t fade = *reinterpret_cast<const uint8_t*>(o.obj + 0x8A))
        w = w + static_cast<float>(fade) * (1.0f / 255.0f);
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = w;
    return true;
}

// FUN_38036D50, the fade factor: 1, unless the object fades (+0x84 & 0xFF00) and the resources' flag lacks bit 4; then
// (1 - (+0x85)/255) * 4 clamped to [0, 1] in the original's order.
float Fade(const PiShared& s, uintptr_t obj)
{
    if (!(*reinterpret_cast<const uint32_t*>(obj + 0x84) & 0xFF00) || (s.matFlag & 4))
        return 1.0f;
    const float t = 1.0f - static_cast<float>(*reinterpret_cast<const uint8_t*>(obj + 0x85)) * (1.0f / 255.0f);
    const float a = t > 0.0f ? t : 0.0f;
    if (a * 4.0f > 1.0f)
        return 1.0f;
    return a * 4.0f;
}

// PI_ObjectAmbColComp (case 0x35): x = +0x3C of the matrix block, y = the ambient scale times the object's opacity
// (+0xF8), times the fade factor when the object's +0x84 is set, z = 0, w = the object's +0xE4.
bool ObjectAmbColComp(const PiShared& s, const PiObject& o, float* out)
{
    if (!o.obj || !o.matrix)
        return false;
    out[0] = *reinterpret_cast<const float*>(o.matrix + 0x3C);
    float y = s.scale * *reinterpret_cast<const float*>(o.obj + 0xF8);
    if (*reinterpret_cast<const int*>(o.obj + 0x84) != 0)
        y = y * Fade(s, o.obj);
    out[1] = y;
    out[2] = 0.0f;
    out[3] = *reinterpret_cast<const float*>(o.obj + 0xE4);
    return true;
}

// PI_AmbientOpacity (case 0x41): xyz = +0x30..0x38 of the matrix block, w = opacity * ambient scale (times the fade
// factor as above); with resources, xyz times the material's ambient colour (+0x118 block [0..2]) unless renderer +0xB20,
// plus its [8..10], times the ambient scale when the resources' +8 has flag 4; then, with the globals 0x382E98B4 and
// 0x382E98C8 set, 0.25 (xmm9 at the setter's entry) added to xyz while the post-effect parameter "NightVision_Active"
// (looked up once by the original into 0x382DAB08, flag 2 of 0x382DAEA8) is not 0. Before that lookup: the original.
bool AmbientOpacity(const PiShared& s, const PiObject& o, float* out)
{
    if (!o.obj || !o.matrix || (s.nightCheck && !s.nightKnown))
        return false;
    const auto f = [](uintptr_t a) { return *reinterpret_cast<const float*>(a); };
    float x = f(o.matrix + 0x30), y = f(o.matrix + 0x34), z = f(o.matrix + 0x38);
    float w = f(o.obj + 0xF8) * s.scale;
    if (*reinterpret_cast<const int*>(o.obj + 0x84) != 0)
        w = w * Fade(s, o.obj);
    if (s.res)
    {
        const uintptr_t amb = *reinterpret_cast<const uintptr_t*>(s.res + 0x118);
        if (!amb)
            return false;
        if (s.b20 == 0)
        {
            x = x * f(amb);
            y = y * f(amb + 4);
            z = z * f(amb + 8);
        }
        x = x + f(amb + 0x20);
        y = y + f(amb + 0x24);
        z = z + f(amb + 0x28);
        if (*reinterpret_cast<const uint8_t*>(s.res + 8) & 4)
        {
            x = x * s.scale;
            y = y * s.scale;
            z = z * s.scale;
        }
        if (s.nightCheck && s.nightActive)
        {
            x = x + 0.25f;
            y = y + 0.25f;
            z = z + 0.25f;
        }
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
    return true;
}

// The per-instance types computed here; false for any other.
bool OwnPiValue(int type, int floats, const PiShared& s, const PiObject& o, float* out)
{
    if (type == g_tObjMatrix && floats == 12)
        return ObjMatrix(s, o, out);
    float v[4];
    if (floats < 1 || floats > 4 ||
        !(type == g_tBendInfo            ? BendInfo(s, o, v)
          : type == g_tAlphaTest         ? AlphaTest(s, o, v)
          : type == g_tObjectAmbColComp  ? ObjectAmbColComp(s, o, v)
          : type == g_tAmbientOpacity    ? AmbientOpacity(s, o, v)
                                         : false))
        return false;
    memcpy(out, v, static_cast<size_t>(floats) * sizeof(float));
    return true;
}

// Constant buffer writes captured while a vf7 runs (the per-batch group), for the vf7 reuse verifier.
struct CbWrite
{
    int reg, slot, stage, floats;
    float v[64];
};
std::vector<CbWrite>* g_cbCapture;

void __fastcall CbWriteHook(int reg, int slot, int stage, const void* data, int floats, int cbVectors)
{
    if (g_cbCapture && floats > 0 && floats <= 64)
    {
        CbWrite w{reg, slot, stage, floats, {}};
        memcpy(w.v, data, static_cast<size_t>(floats) * sizeof(float));
        g_cbCapture->push_back(w);
    }
    if (g_capturing)
        for (int i = 0; i < g_expectCount; i++)
        {
            Expect& e = g_expect[i];
            if (e.seen || e.reg != reg || e.slot != slot || e.stage != stage)
                continue;
            e.seen = true;
            g_piVerified++;
            const int n = floats < e.floats ? floats : e.floats;
            if (n != e.floats || memcmp(e.v, data, static_cast<size_t>(n) * sizeof(float)) != 0)
            {
                g_piMismatches++;
                if (g_mismatchLog.size() < 4000)
                {
                    char line[400];
                    const auto* d = static_cast<const float*>(data);
                    int len = snprintf(line, sizeof(line), "  stage %d slot %d reg %d floats %d/%d\n    ours:", stage, slot, reg, e.floats, floats);
                    for (int k = 0; k < e.floats && len < 300; k++)
                        len += snprintf(line + len, sizeof(line) - len, " %g", e.v[k]);
                    len += snprintf(line + len, sizeof(line) - len, "\n    orig:");
                    for (int k = 0; k < floats && k < 16 && len < 390; k++)
                        len += snprintf(line + len, sizeof(line) - len, " %g", d[k]);
                    g_mismatchLog += line;
                    g_mismatchLog += "\n";
                }
            }
            break;
        }
    g_cbWrite(reg, slot, stage, data, floats, cbVectors);
}

// ---------------------------------------------------------------------------------------------------------------------
// Draw packets (CRYMT_ZPACKETS=1, D3D12 back end): inside the per-object loop (FUN_380268D0), a per-instance group we
// compute is held back instead of written; CREMesh's draw then does what FUN_38024D80 does for a triangle list, but
// instead of the engine's per-object Map/Unmap/bind/DrawIndexed it records one packet (DrawPacket.h) with the held
// constants. Held constants that no packet takes are written the engine's way, so nothing is lost on other paths.
// ---------------------------------------------------------------------------------------------------------------------
using MeshDrawFn = uint64_t(__fastcall*)(uintptr_t re, uintptr_t p2, uintptr_t p3);
using CommitFn = void(__fastcall*)(char);
using SetTargetsFn = void(__fastcall*)(uintptr_t renderer, int);

MeshDrawFn g_meshDraw;
bool g_packets;
int g_packetsReady = -1; // -1 not checked yet
int g_inObjects;         // depth inside FUN_380268D0
constexpr int kMaxHeldVectors = 64;
struct Held
{
    bool active;
    int slot, cbVectors, n;
    int reg[kMaxGroupParams], floats[kMaxGroupParams];
    alignas(16) float data[kMaxHeldVectors * 4];
};
Held g_held[3];
uint64_t g_packetsSent, g_packetFallbacks, g_heldFlushed, g_heldDropped;

void FlushHeld(int stage)
{
    Held& h = g_held[stage];
    if (!h.active)
        return;
    for (int i = 0; i < h.n; i++)
        g_cbWrite(h.reg[i], h.slot, stage, h.data + h.reg[i], h.floats[i], h.cbVectors);
    h.active = false;
    g_heldFlushed++;
}

void FlushAllHeld()
{
    for (int s = 0; s < 3; s++)
        FlushHeld(s);
}

bool PacketsReady()
{
    if (g_packetsReady < 0)
        g_packetsReady = D3DLayerPacketsAvailable() ? 1 : 0;
    return g_packetsReady == 1;
}

// Holds a computed group for the next packet; false when it does not fit (written instead).
bool HoldGroup(int stage, const Expect* params, int count, int cbVectors)
{
    if (stage < 0 || stage > 2 || cbVectors <= 0 || cbVectors > kMaxHeldVectors)
        return false;
    for (int i = 0; i < count; i++)
        if (params[i].slot != params[0].slot || params[i].reg < 0 || params[i].reg + params[i].floats > cbVectors * 4)
            return false;
    FlushHeld(stage); // an earlier object's group that no draw took
    Held& h = g_held[stage];
    memset(h.data, 0, static_cast<size_t>(cbVectors) * 16);
    for (int i = 0; i < count; i++)
    {
        memcpy(h.data + params[i].reg, params[i].v, static_cast<size_t>(params[i].floats) * sizeof(float));
        h.reg[i] = params[i].reg;
        h.floats[i] = params[i].floats;
    }
    h.n = count;
    h.slot = params[0].slot;
    h.cbVectors = cbVectors;
    h.active = true;
    return true;
}

// A packet's own geometry (our item path): streams and index buffer taken from the mesh, and what the packet may skip.
struct PacketGeometry
{
    uint32_t streams = 0;
    DrawPacketStream s[kDrawPacketMaxStreams];
    DrawPacketIndex index{};
    uint32_t indexBase = 0; // first index of the mesh's index range (its byte offset / 2)
    bool skipCommit = false, skipTargets = false;
};

uint64_t PacketDraw(uintptr_t r, const PacketGeometry* geo = nullptr);

uint64_t __fastcall MeshDrawHook(uintptr_t re, uintptr_t p2, uintptr_t p3)
{
    CycleScope cycles(kCyMeshDraw);
    if (!g_held[0].active && !g_held[1].active && !g_held[2].active)
        return g_meshDraw(re, p2, p3);
    const uintptr_t r = *reinterpret_cast<const uintptr_t*>(kRendererGlobal);
    const uintptr_t mesh = *reinterpret_cast<const uintptr_t*>(re + 0x48);
    if (!p2 || *reinterpret_cast<const int*>(p2 + 0x60) == 0 || *reinterpret_cast<const int*>(r + kRdChunkStream) >= 0 || !mesh ||
        *reinterpret_cast<const int*>(mesh + kMeshPrimitive) != 0)
    {
        g_packetFallbacks++;
        FlushAllHeld();
        return g_meshDraw(re, p2, p3);
    }
    return PacketDraw(r);
}

// FUN_38024D80 for a triangle list of the prepared mesh (renderer +0x12928..), with the draw recorded as a packet
// that carries the held per-instance groups.
uint64_t PacketDraw(uintptr_t r, const PacketGeometry* geo)
{
    const auto drop = [] {
        for (Held& h : g_held)
            h.active = false;
        g_heldDropped++;
        return uint64_t{1};
    };
    // FUN_38024D80 for a triangle list, with the draw replaced by a packet.
    if (*reinterpret_cast<const int*>(kDrawDisabled))
        return drop();
    if (!(geo && geo->skipCommit))
        reinterpret_cast<CommitFn>(kCommitConstantsFn)(1);
    if (!(geo && geo->skipTargets))
        reinterpret_cast<SetTargetsFn>(kSetTargetsFn)(r, 0);
    const uintptr_t vs = *reinterpret_cast<const uintptr_t*>(kCurVsInstance), ps = *reinterpret_cast<const uintptr_t*>(kCurPsInstance),
                    gs = *reinterpret_cast<const uintptr_t*>(kCurGsInstance);
    if (!vs || !ps || *reinterpret_cast<const char*>(vs + kInstanceNotReady) || *reinterpret_cast<const char*>(ps + kInstanceNotReady) ||
        (gs && *reinterpret_cast<const char*>(gs + kInstanceNotReady)))
        return drop();
    const int count = *reinterpret_cast<const int*>(r + kRdIndexCount);
    if (count / 3 == 0)
        return drop();
    if (*reinterpret_cast<const int*>(r + kRdTopology) != 4)
    {
        *reinterpret_cast<int*>(r + kRdTopology) = 4;
        auto* device = *reinterpret_cast<void***>(r + kRdDevice);
        reinterpret_cast<void(__stdcall*)(void*, int)>((*reinterpret_cast<void***>(device))[0x90 / 8])(device, 4);
    }
    alignas(16) uint8_t packet[kDrawPacketMaxBytes];
    const int indexBase = geo ? static_cast<int>(geo->indexBase) : *reinterpret_cast<const int*>(r + kRdIndexBase);
    DrawPacketHeader head{static_cast<uint32_t>(count), static_cast<uint32_t>(indexBase + *reinterpret_cast<const int*>(r + kRdFirstIndex)), 0, 0, 0, 0, 0};
    uint32_t used = sizeof(head);
    for (int s = 0; s < 3; s++)
    {
        Held& h = g_held[s];
        if (!h.active)
            continue;
        const uint32_t bytes = static_cast<uint32_t>(h.cbVectors) * 16;
        if (head.constants < kDrawPacketMaxConstants && used + sizeof(DrawPacketConstants) + bytes <= sizeof(packet))
        {
            const DrawPacketConstants c{static_cast<uint8_t>(s), static_cast<uint8_t>(h.slot), static_cast<uint16_t>(h.cbVectors)};
            memcpy(packet + used, &c, sizeof(c));
            memcpy(packet + used + sizeof(c), h.data, bytes);
            used += sizeof(c) + bytes;
            head.constants++;
            h.active = false;
        }
        else
            h.active = false; // cannot happen: three held groups of at most 64 vectors fit a packet
    }
    void* objects[kDrawPacketMaxStreams + 1];
    int held = 0;
    if (geo)
    {
        for (uint32_t i = 0; i < geo->streams; i++)
        {
            memcpy(packet + used, &geo->s[i], sizeof(geo->s[i]));
            used += sizeof(geo->s[i]);
            if (geo->s[i].buffer)
                objects[held++] = reinterpret_cast<void*>(geo->s[i].buffer);
        }
        head.streams = static_cast<uint8_t>(geo->streams);
        memcpy(packet + used, &geo->index, sizeof(geo->index));
        used += sizeof(geo->index);
        head.indexBuffer = 1;
        if (geo->index.buffer)
            objects[held++] = reinterpret_cast<void*>(geo->index.buffer);
    }
    memcpy(packet, &head, sizeof(head));
    D3DLayerDrawPacket(packet, used, objects, held);
    const int list = *reinterpret_cast<const int*>(r + kRdStatList);
    *reinterpret_cast<int*>(r + kRdStatTriangles + static_cast<uintptr_t>(list) * 4) += count / 3;
    *reinterpret_cast<int*>(r + kRdStatDraws + static_cast<uintptr_t>(list) * 4) += 1;
    g_packetsSent++;
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// vf7 reuse (CRYMT_SETREUSE=1; CRYMT_SETVERIFY=1 checks it): a CHWShader_D3D vf7 (shader set) call with the shader,
// flags, masks, material, recursion level, frame and view-projection of the previous vf7 call for that stage selects
// the same instance, binds nothing new and writes the same per-batch constants, so only its renderer field writes are
// repeated. Not reused: pixel shaders with flag 8 (their second lookup depends on FUN_3803C810's side effects) and
// the shader debug modes.
// ---------------------------------------------------------------------------------------------------------------------
using ShaderSetFn = uint64_t(__fastcall*)(uintptr_t shader, uint32_t flags);
ShaderSetFn g_shaderSet;
bool g_setReuse, g_setVerify;

struct SetKey
{
    uintptr_t shader, resources;
    uint64_t rt;
    uint32_t flags, lt, gl, md, mdv, level, frame, pad;
    float viewProj[16];
    bool operator==(const SetKey& o) const
    {
        return memcmp(this, &o, sizeof(*this)) == 0;
    }
};
struct SetMemo
{
    bool valid;
    SetKey key;
    uintptr_t inst, shaderInst; // the stage's current instance global, and the shader's own (+0x70)
    int pbSlot;                 // the per-batch group's constant buffer slot (-1: none) ...
    uintptr_t pbCb;             // ... and the buffer it was written to
    std::vector<CbWrite> pb;
};
constexpr uintptr_t kCbObjects = 0x382D8320; // [stage * 9 + slot]: buffer of the current size
constexpr uintptr_t kCbMapped = 0x382D8400;  // mapped pointer while a write is pending
constexpr uintptr_t kCbBound = 0x382D84E0;   // buffer bound to the device

// The skipped per-batch constants are still valid only while their buffer is the one bound and nothing is pending.
bool MemoBufferValid(const SetMemo& m, uint32_t type)
{
    if (m.pbSlot < 0)
        return true;
    const uintptr_t i = (static_cast<uintptr_t>(type) * 9 + static_cast<uintptr_t>(m.pbSlot)) * 8;
    return *reinterpret_cast<const uintptr_t*>(kCbBound + i) == m.pbCb && *reinterpret_cast<const uintptr_t*>(kCbMapped + i) == 0;
}
SetMemo g_setMemo[3];
uint64_t g_vf7Calls, g_setReused, g_setCandidates, g_setInstDiff, g_setPbDiff, g_setD3dDiff;
constexpr uintptr_t kShaderDebugMode = 0x382E98F8; // 1..3: vf7 takes a debug path
constexpr uintptr_t kRdViewProj = 0x190;
const uintptr_t kCurInstance[3] = {kCurVsInstance, kCurPsInstance, kCurGsInstance};

bool MakeSetKey(uintptr_t shader, uint32_t flags, SetKey& k)
{
    const uintptr_t r = *reinterpret_cast<const uintptr_t*>(kRendererGlobal);
    const uint32_t type = *reinterpret_cast<const uint32_t*>(shader + 0x20);
    if (static_cast<uint32_t>(*reinterpret_cast<const int*>(kShaderDebugMode)) - 1u < 3u || type > 2 || (type == 1 && (flags & 8)))
        return false;
    memset(&k, 0, sizeof(k));
    // The masks exactly as vf7 computes them.
    uint32_t lt = *reinterpret_cast<const uint32_t*>(r + 0xB08);
    if (lt)
    {
        const uint32_t u = *reinterpret_cast<const uint32_t*>(shader + 0x5C);
        if (!(u & 0x40060))
            lt = 0;
        else if (!(u & 0x40) && (u & 0x20) && (lt & 0x300) != 0x200)
            lt = 1;
    }
    uint32_t md = *reinterpret_cast<const uint32_t*>(r + 0xB18), mdv = *reinterpret_cast<const uint32_t*>(r + 0xB1C);
    if (type == 1)
    {
        md &= 0xFF;
        mdv = 0;
    }
    k.shader = shader;
    k.resources = *reinterpret_cast<const uintptr_t*>(r + kRdShaderRes);
    k.rt = *reinterpret_cast<const uint64_t*>(shader + 0x48) & *reinterpret_cast<const uint64_t*>(r + 0xB10);
    k.flags = flags;
    k.lt = lt;
    k.gl = *reinterpret_cast<const uint32_t*>(shader + 0x50);
    k.md = md;
    k.mdv = mdv;
    k.level = *reinterpret_cast<const uint32_t*>(kRecurseLevel);
    k.frame = *reinterpret_cast<const uint32_t*>(r + kRdFrameId);
    memcpy(k.viewProj, reinterpret_cast<const void*>(r + kRdViewProj), sizeof(k.viewProj));
    return true;
}

// vf7's effects for an instance it already selected and bound (flags without 4: the binding branch).
void ApplyMemo(uint32_t type, uint32_t flags, uintptr_t inst)
{
    if (flags & 4)
        return;
    *reinterpret_cast<uintptr_t*>(kCurInstance[type]) = inst;
    if (type != 0)
        return;
    const uintptr_t r = *reinterpret_cast<const uintptr_t*>(kRendererGlobal);
    if (*reinterpret_cast<const uintptr_t*>(r + kRdRendElement))
        *reinterpret_cast<uint32_t*>(r + 0x180C) = *reinterpret_cast<const uint8_t*>(inst + 0x8B);
    uint32_t b00 = *reinterpret_cast<const uint16_t*>(inst + 0x84), b04 = *reinterpret_cast<const uint16_t*>(inst + 0x82);
    if (flags & 0x20)
    {
        b00 &= 0xFFFFFEFB;
        b04 &= 0xFFFFFEFB;
    }
    *reinterpret_cast<uint32_t*>(r + 0xB00) = b00;
    *reinterpret_cast<uint32_t*>(r + 0xB04) = b04;
}

uint64_t __fastcall ShaderSetHook(uintptr_t shader, uint32_t flags)
{
    CycleScope cycles(kCyShaderSet);
    g_vf7Calls++;
    SetKey k;
    const bool keyed = MakeSetKey(shader, flags, k);
    const uint32_t type = *reinterpret_cast<const uint32_t*>(shader + 0x20);
    SetMemo* m = keyed ? &g_setMemo[type] : nullptr;
    const bool candidate = m && m->valid && m->key == k && MemoBufferValid(*m, type) &&
                           *reinterpret_cast<const uintptr_t*>(shader + 0x70) == m->shaderInst;
    if (candidate && g_setReuse)
    {
        ApplyMemo(type, flags, m->inst);
        g_setReused++;
        return 1;
    }
    std::vector<CbWrite> writes;
    std::vector<uint32_t> ops;
    if (g_setVerify)
    {
        g_cbCapture = &writes;
        if (candidate)
            D3DLayerTrace(&ops);
    }
    const uint64_t ret = g_shaderSet(shader, flags);
    if (g_setVerify)
    {
        g_cbCapture = nullptr;
        if (candidate)
        {
            D3DLayerTrace(nullptr);
            g_setCandidates++;
            const uintptr_t inst = (flags & 4) ? m->inst : *reinterpret_cast<const uintptr_t*>(kCurInstance[type]);
            g_setInstDiff += inst != m->inst || ret != 1 || *reinterpret_cast<const uintptr_t*>(shader + 0x70) != m->shaderInst;
            bool pbSame = writes.size() == m->pb.size();
            for (size_t i = 0; pbSame && i < writes.size(); i++)
            {
                const CbWrite &a = writes[i], &b = m->pb[i];
                pbSame = a.reg == b.reg && a.slot == b.slot && a.stage == b.stage && a.floats == b.floats &&
                         memcmp(a.v, b.v, static_cast<size_t>(a.floats) * sizeof(float)) == 0;
            }
            g_setPbDiff += !pbSame;
            bool d3dSame = true;
            for (const uint32_t op : ops)
                d3dSame = d3dSame && (op & 0xFFFF) >= 0x100; // only uploads, no device state call
            g_setD3dDiff += !d3dSame;
        }
    }
    if (m)
    {
        m->valid = ret == 1;
        m->key = k;
        m->inst = (flags & 4) ? 0 : *reinterpret_cast<const uintptr_t*>(kCurInstance[type]);
        m->valid = m->valid && ((flags & 4) || m->inst);
        m->shaderInst = *reinterpret_cast<const uintptr_t*>(shader + 0x70);
        m->pbSlot = -1;
        m->pbCb = 0;
        const auto* range = m->shaderInst ? *reinterpret_cast<const uintptr_t* const*>(m->shaderInst + 0x38) : nullptr;
        if (m->valid && !(flags & 4) && range && range[1] > range[0])
        {
            const int slot = *reinterpret_cast<const int16_t*>(range[0] + 0xE);
            if (slot >= 0 && slot < 9)
            {
                m->pbSlot = slot;
                m->pbCb = *reinterpret_cast<const uintptr_t*>(kCbObjects + (static_cast<uintptr_t>(type) * 9 + static_cast<uintptr_t>(slot)) * 8);
            }
            else
                m->valid = false;
        }
        if (g_setVerify)
            m->pb.swap(writes);
    }
    return ret;
}

void __fastcall SetGroupHook(uintptr_t shader, int group)
{
    CycleScope cycles(group == 1 ? kCySetGroup : group == 0 ? kCySetGroup0 : -1);
    if (!g_piFast && !g_piVerify)
    {
        g_setGroup(shader, group);
        return;
    }
    const uintptr_t inst = group == 1 ? *reinterpret_cast<const uintptr_t*>(shader + 0x70) : 0;
    const auto* range = inst ? *reinterpret_cast<const uintptr_t* const*>(inst + 0x38 + static_cast<uintptr_t>(group) * 8) : nullptr;
    if (!range)
    {
        g_setGroup(shader, group);
        return;
    }
    const uint64_t entryTicks = __rdtsc();
    ResolveTypes();
    g_piCalls++;
    const uintptr_t begin = range[0];
    const int count = static_cast<int>((range[1] - begin) / kParamSize);
    if (count <= 0 || count > kMaxGroupParams)
    {
        g_piUnsupported[-1]++;
        g_setGroup(shader, group);
        return;
    }
    const int stage = *reinterpret_cast<const int*>(shader + 0x20);
    const int cbVectors = *reinterpret_cast<const int*>(inst + 0x70 + static_cast<uintptr_t>(group) * 4);
    PiShared piShared;
    PiObject piObject;
    CapturePi(piShared, piObject);
    Expect local[kMaxGroupParams];
    for (int i = 0; i < count; i++)
    {
        const uintptr_t p = begin + static_cast<uintptr_t>(i) * kParamSize;
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(p + 8);
        const int type = static_cast<int>(*reinterpret_cast<const uint32_t*>(p + 0x18) & 0xFF);
        Expect& e = local[i];
        e.reg = *reinterpret_cast<const int16_t*>(p + 0xC);
        e.slot = *reinterpret_cast<const int16_t*>(p + 0xE);
        e.stage = stage;
        e.floats = *reinterpret_cast<const int32_t*>(p + 0x10);
        e.seen = false;
        // Written only when the register is valid or flag 8 asks for it, as the original does.
        const bool written = (flags & 8) || e.reg >= 0;
        const bool ok = (flags & 0x40000) && written && OwnPiValue(type, e.floats, piShared, piObject, e.v);
        if (!ok)
        {
            g_piUnsupported[type]++;
            g_setGroup(shader, group);
            return;
        }
    }
    if (g_piVerify)
    {
        memcpy(g_expect, local, sizeof(Expect) * static_cast<size_t>(count));
        g_expectCount = count;
        g_capturing = true;
        const uint64_t t0 = __rdtsc();
        g_setGroup(shader, group);
        g_piTicks += __rdtsc() - t0;
        g_piTimed++;
        g_capturing = false;
        for (int i = 0; i < count; i++)
            g_piUnseen += !g_expect[i].seen;
        return;
    }
    if (!(g_packets && g_inObjects > 0 && PacketsReady() && HoldGroup(stage, local, count, cbVectors)))
        for (int i = 0; i < count; i++)
            g_cbWrite(local[i].reg, local[i].slot, stage, local[i].v, local[i].floats, cbVectors);
    g_piTicks += __rdtsc() - entryTicks;
    g_piTimed++;
    g_piFast_++;
}

// ---------------------------------------------------------------------------------------------------------------------
// Own z-prepass lists (CRYMT_ZOWN=1 with CRYMT_D3D12; CRYMT_ZOWNVERIFY=1 also runs the original on our items, a second
// depth-only draw being harmless, and checks that it sets no state ours left out). The z-prepass's six
// FX_ProcessBatchesList calls come here. Items go in runs of equal sort keys: a group of consecutive items sharing a
// render element goes to the original (which may merge it for instancing); when that group was one item drawn by
// one plain per-object loop, it leaves the run's shaders, per-batch constants, textures and states set, and the next
// single items of the run that are static triangle-list CREMesh with the pass-flag inputs and vertex format of the
// run's object are drawn by us: what the original does per item, without what is the same for the whole run
// (technique, pass flags, both vf7, per-batch constants, PS resources, the flush).
// ---------------------------------------------------------------------------------------------------------------------
using ListFn = void(__fastcall*)(uintptr_t renderer, int first, int last, uint32_t filter);
using ObjChangeFn = char(__fastcall*)(uintptr_t renderer, uintptr_t shader, uintptr_t resources, uintptr_t object, uintptr_t re);
using BatchStartFn = void(__fastcall*)(uintptr_t renderer, uintptr_t shader, uint32_t technique, uintptr_t resources, uintptr_t re);
using DeclSetupFn = uint32_t(__fastcall*)(uintptr_t renderer);
using DeclSetFn = int(__fastcall*)(uintptr_t renderer, uint32_t b00, uint32_t f180c);
using DeclRestoreFn = void(__fastcall*)(uintptr_t renderer, const void* saved);
using ReFn0 = uint64_t(__fastcall*)(uintptr_t re);
using ReFn1 = uint64_t(__fastcall*)(uintptr_t re, uintptr_t a);
using ReFn2 = uint64_t(__fastcall*)(uintptr_t re, uintptr_t a, uintptr_t b);

bool g_zown, g_zownVerify;
bool g_zownEngineDraw; // CRYMT_ZOWNDRAW=engine: our item setup, the engine's draw (to tell setup from packet problems)
bool g_zownFlushDraw;  // CRYMT_ZOWNDRAW=flush: our object change and prepare, then the original batch flush
enum ZReject
{
    kRejGroup,
    kRejRun,
    kRejNotMesh,
    kRejFormat,
    kRejObject,
    kRejInstances,
    kRejSkinned,
    kRejPrepare,
    kRejPiTypes,
    kRejCount
};
const char* const kRejectNames[kRejCount] = {"run base not a single plain item", "run without shader/mesh/VS", "not a triangle-list CREMesh",
                                             "other vertex format",            "other object pass inputs",   "objdata instance matrices",
                                             "skinned",                       "prepare gave another path",  "run with unknown per-instance types"};
uint64_t g_zItems, g_zOurs, g_zSkipped, g_zReject[kRejCount];
uint64_t g_zRunWhy[4]; // no shader, no object, not a CREMesh, no VS
uint64_t g_zBaseGroups, g_zBaseItems, g_zRuns, g_zRunLen[16]; // list structure: original groups, sort-key runs by length
// Verify mode: constant buffer writes of the run's base group, and mismatches against the original's writes for our items.
std::vector<CbWrite> g_zBaseWrites, g_zOurPi;
uint64_t g_zPiCompared, g_zPiMismatch, g_zPbCompared, g_zPbMismatch, g_zPbMissing;
std::string g_zMismatchLog, g_zNoDrawLog, g_zNotWrittenLog;
uint64_t g_zOrigNoDraw, g_zPiNotWritten;
uint64_t g_zOwnStreams, g_zCommitSkipped, g_zStreamsCompared, g_zStreamsMismatch;
std::unordered_map<uintptr_t, uintptr_t> g_zSeenVt; // render element vtables of rejected runs -> slot 15
std::unordered_map<std::string, uint64_t> g_zVerifyOps;
uint64_t g_zVerified;

struct ZRun
{
    bool ok;
    uint32_t sortKey;
    uintptr_t shader, resources, pass, loopArg, stageShader[3]; // stage shaders in the renderer's numbering: VS, PS, GS
    uint32_t vertexFormat, objFlags, objC0, objF8, obj84;
    uint8_t obj8A;
};

// Points the E8 call at site (calling target) at hook, through a jump in a page near the module.
bool PatchCall(uintptr_t site, uintptr_t target, const void* hook)
{
    auto* p = reinterpret_cast<uint8_t*>(site);
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    if (p[0] != 0xE8 || site + 5 + static_cast<intptr_t>(rel) != target)
        return false;
    uint8_t* cave = nullptr;
    for (uintptr_t hint = (site & ~uintptr_t{0xFFFF}) + 0x4000000; hint < site + 0x40000000 && !cave; hint += 0x10000)
        cave = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!cave)
        return false;
    const auto h = reinterpret_cast<uint64_t>(hook);
    cave[0] = 0x48;
    cave[1] = 0xB8;
    memcpy(cave + 2, &h, 8);
    cave[10] = 0xFF;
    cave[11] = 0xE0;
    DWORD old;
    VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &old);
    rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(cave) - static_cast<intptr_t>(site + 5));
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

uintptr_t Vt(uintptr_t object, int slot)
{
    return reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(object))[slot];
}

// By the vtable pointer: in fill mode the entries are CryMT's thunks.
bool IsMeshRe(uintptr_t re)
{
    return re && *reinterpret_cast<const uintptr_t*>(re) == kCREMeshVtable;
}

// Shader and resources of a sort key, as FX_ProcessBatchesList reads them.
void KeyShader(uint32_t key, uintptr_t& shader, uintptr_t& resources)
{
    const uintptr_t table = **reinterpret_cast<const uintptr_t* const*>(kShaderTable); // pointer to the array's pointer
    const uintptr_t entry = table ? *reinterpret_cast<const uintptr_t*>(table + static_cast<uintptr_t>(key >> 6 & 0xFFF) * 8) : 0;
    shader = entry ? entry - 8 : 0;
    resources = key >> 18 ? *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(kShaderResTable) + static_cast<uintptr_t>(key >> 18) * 8) : 0;
}

// The object's inputs to the pass flags (FUN_3801BA60), the batch start and the MDV mask (FUN_38021530), as they are
// used: flags; +0xC0 and, when it is set, +0x8A; opacity only as "below 1"; +0x84/+0x85 only as the outcome of the
// alpha test; +0xC8 (ORed into the MDV mask).
void ObjectInputs(uintptr_t obj, uint32_t& flags, uint32_t& c0, uint32_t& f8, uint32_t& o84, uint8_t& o8a)
{
    flags = *reinterpret_cast<const uint32_t*>(obj + 0x10);
    c0 = *reinterpret_cast<const uint32_t*>(obj + 0xC0);
    o8a = c0 ? *reinterpret_cast<const uint8_t*>(obj + 0x8A) : 0;
    f8 = *reinterpret_cast<const float*>(obj + 0xF8) < 1.0f ? 1u : 0u;
    const bool fade = (*reinterpret_cast<const uint32_t*>(obj + 0x84) & 0xFF00) != 0 &&
                      *reinterpret_cast<const float*>(0x3824D190) < static_cast<float>(*reinterpret_cast<const uint8_t*>(obj + 0x85)) *
                                                                         *reinterpret_cast<const float*>(0x3824D194);
    o84 = (fade ? 1u : 0u) | static_cast<uint32_t>((*reinterpret_cast<const uint32_t*>(obj + 0x84) & 0xFF00) != 0) << 1 |
          *reinterpret_cast<const uint32_t*>(obj + 0xC8) << 2;
}

// The per-instance group of a stage shader: computed values (compute false: only checked), or -1 when a parameter
// cannot be taken (0: no group).
int ComputePiGroup(uintptr_t shader, Expect* out, int& stage, int& cbVectors, bool compute = true)
{
    const uintptr_t inst = *reinterpret_cast<const uintptr_t*>(shader + 0x70);
    const auto* range = inst ? *reinterpret_cast<const uintptr_t* const*>(inst + 0x38 + 8) : nullptr;
    if (!range || range[1] <= range[0])
        return 0;
    const int count = static_cast<int>((range[1] - range[0]) / kParamSize);
    if (count > kMaxGroupParams)
        return -1;
    stage = *reinterpret_cast<const int*>(shader + 0x20);
    cbVectors = *reinterpret_cast<const int*>(inst + 0x70 + 4);
    PiShared piShared;
    PiObject piObject;
    if (compute)
        CapturePi(piShared, piObject);
    int n = 0;
    for (int i = 0; i < count; i++)
    {
        const uintptr_t p = range[0] + static_cast<uintptr_t>(i) * kParamSize;
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(p + 8);
        const int type = static_cast<int>(*reinterpret_cast<const uint32_t*>(p + 0x18) & 0xFF);
        Expect& e = out[n];
        e.reg = *reinterpret_cast<const int16_t*>(p + 0xC);
        e.slot = *reinterpret_cast<const int16_t*>(p + 0xE);
        e.stage = stage;
        e.floats = *reinterpret_cast<const int32_t*>(p + 0x10);
        e.seen = false;
        if (!((flags & 8) || e.reg >= 0))
            continue; // the original does not write it either
        if (e.floats <= 0 || e.floats > 16)
            return -1;
        if (!compute)
        {
            n++;
            continue;
        }
        if (!((flags & 0x40000) && OwnPiValue(type, e.floats, piShared, piObject, e.v)))
        {
            // Every other type through the engine's own setter, into memory (its instancing path copies each
            // parameter's floats * 16 bytes from its scratch; the first floats are the value).
            alignas(16) float chunk[16 * 4];
            reinterpret_cast<SetParamsFn>(kSetParamsFn)(p, 1, chunk, stage, static_cast<uint32_t>(cbVectors));
            memcpy(e.v, chunk, static_cast<size_t>(e.floats) * sizeof(float));
        }
        n++;
    }
    return n;
}

// CREMesh vf17's streams for the stream mask at renderer +0xB04 (set from the VS instance), without binding anything:
// stream 0 from the vertex source mesh (mesh +0x80, else the mesh) container at +0x78, streams 1..6 (mask bits 2 << n)
// from the containers that follow (0x20 apart), the index buffer (R16) from the mesh. False for what we leave to vf17:
// the per-object stream 2 (bit 4) and the secondary set (bit 0x100), or no vertex container.
bool ExtractStreams(uintptr_t r, uintptr_t re, PacketGeometry& g)
{
    const uintptr_t mesh = *reinterpret_cast<const uintptr_t*>(re + 0x48);
    const uintptr_t alt = *reinterpret_cast<const uintptr_t*>(mesh + 0x80);
    const uintptr_t src = alt ? alt : mesh;
    const uintptr_t vbc = *reinterpret_cast<const uintptr_t*>(src + 0x78);
    const uint32_t mask = *reinterpret_cast<const uint32_t*>(r + 0xB04);
    if (!vbc || (mask & 0xFFFFFF84u))
        return false;
    const auto buffer = [](uintptr_t c) {
        const uintptr_t shared = *reinterpret_cast<const uintptr_t*>(c + 0x18);
        return !shared || *reinterpret_cast<const uint8_t*>(c + 0x11) ? *reinterpret_cast<const uintptr_t*>(c + 8)
                                                                      : *reinterpret_cast<const uintptr_t*>(shared + 8);
    };
    g.streams = 0;
    uintptr_t b = buffer(vbc);
    if (!b)
        b = *reinterpret_cast<const uintptr_t*>(r + 0x2A150);
    const int format = *reinterpret_cast<const int*>(src + kMeshVertexFormat);
    g.s[g.streams++] = {b, *reinterpret_cast<const uint32_t*>(vbc + 0x14), *reinterpret_cast<const uint32_t*>(0x38269B30 + static_cast<uintptr_t>(format) * 4), 0, 0};
    for (uint32_t i = 1; i < 7; i++)
    {
        if (!(mask & (2u << (i - 1))))
            continue;
        const uintptr_t c = vbc + i * 0x20;
        b = buffer(c);
        if (!b && i == 1)
            b = *reinterpret_cast<const uintptr_t*>(r + 0x2A160);
        g.s[g.streams++] = {b, *reinterpret_cast<const uint32_t*>(c + 0x14), *reinterpret_cast<const uint32_t*>(0x38269B6C + (i - 1) * 4), i, 0};
    }
    const uintptr_t sharedIb = *reinterpret_cast<const uintptr_t*>(mesh + 0xE0);
    const uintptr_t ib = !sharedIb || *reinterpret_cast<const uint8_t*>(mesh + 0xD9) ? *reinterpret_cast<const uintptr_t*>(mesh + 0xD0)
                                                                                    : *reinterpret_cast<const uintptr_t*>(sharedIb + 8);
    g.index = {ib, 57 /* DXGI_FORMAT_R16_UINT */, 0};
    g.indexBase = static_cast<uint32_t>(*reinterpret_cast<const int*>(mesh + 0xDC) >> 1);
    return true;
}

// Whether FUN_380452F0 (the per-draw constant commit) would do anything for this draw: matrices marked dirty,
// a pending or unbound buffer in the 27 stage/slot pairs, the material buffers of instances that use them (or the
// object's own data for the VS), the light buffers.
uint64_t g_commitWhy[5]; // dirty matrices, pending, unbound, material/object data, light
bool CommitNeeded(uintptr_t r, uintptr_t obj)
{
    if (*reinterpret_cast<const uint32_t*>(r + 0xAF8) & 0x202000)
        return g_commitWhy[0]++, true;
    // FUN_3801B7A0 does nothing for a slot without a pending write (it binds only after unmapping one).
    for (uintptr_t i = 0; i < 27; i++)
        if (*reinterpret_cast<const uintptr_t*>(kCbMapped + i * 8))
            return g_commitWhy[1]++, true;
    const auto at = [](uintptr_t a) { return *reinterpret_cast<const uintptr_t*>(a); };
    if (const uintptr_t res = at(r + kRdShaderRes))
    {
        const uintptr_t vs = at(kCurVsInstance), ps = at(kCurPsInstance), gs = at(kCurGsInstance);
        if (vs && *reinterpret_cast<const uint8_t*>(vs + 0x89))
        {
            const uintptr_t od = reinterpret_cast<ReFn1>(Vt(obj, 1))(obj, 0);
            const uintptr_t data = od ? at(od + 0x18) : 0;
            if ((data && *reinterpret_cast<const int*>(data - 4) != 0) || at(0x382D84F8) != at(res + 0x128))
                return g_commitWhy[3]++, true;
        }
        if (ps && *reinterpret_cast<const uint8_t*>(ps + 0x89) && at(0x382D8540) != at(res + 0x130))
            return g_commitWhy[3]++, true;
        if (gs && *reinterpret_cast<const uint8_t*>(gs + 0x89) && at(0x382D8588) != at(res + 0x138))
            return g_commitWhy[3]++, true;
    }
    if ((at(0x382D8388) && at(0x382D8548) != at(0x382D8388)) || (at(0x382D8340) && at(0x382D8500) != at(0x382D8340)) ||
        (at(0x382D83D0) && at(0x382D8590) != at(0x382D83D0)))
        return g_commitWhy[4]++, true;
    return false;
}

// After the original drew the run's base group (one item, one plain per-object loop): what our items must match.
void BeginRun(uintptr_t r, uintptr_t item, bool plainSingle, ZRun& run)
{
    run.ok = false;
    run.sortKey = *reinterpret_cast<const uint32_t*>(item);
    if (!plainSingle || !g_lastPass)
    {
        g_zReject[kRejGroup]++;
        return;
    }
    KeyShader(run.sortKey, run.shader, run.resources);
    const uintptr_t re = *reinterpret_cast<const uintptr_t*>(item + 8), obj = *reinterpret_cast<const uintptr_t*>(item + 0x18);
    run.pass = g_lastPass;
    run.loopArg = g_lastLoopArg;
    run.stageShader[0] = *reinterpret_cast<const uintptr_t*>(run.pass + 8);    // VS
    run.stageShader[1] = *reinterpret_cast<const uintptr_t*>(run.pass + 0x10); // PS
    run.stageShader[2] = *reinterpret_cast<const uintptr_t*>(run.pass + 0x18); // GS
    const int why = !run.shader ? 0 : !obj ? 1 : !IsMeshRe(re) ? 2 : !run.stageShader[0] ? 3 : -1;
    if (why >= 0)
    {
        g_zReject[kRejRun]++;
        g_zRunWhy[why]++;
        if (why == 2 && re && g_zSeenVt.size() < 12)
            g_zSeenVt[*reinterpret_cast<const uintptr_t*>(re)] = Vt(re, 15);
        return;
    }
    // Per-instance groups of known types only (they depend on the instances, the same for the whole run).
    for (const uintptr_t s : run.stageShader)
    {
        Expect probe[kMaxGroupParams];
        int stage, cbVectors;
        if (s && (ComputePiGroup(s, probe, stage, cbVectors, false) < 0 || stage < 0 || stage > 2))
        {
            g_zReject[kRejPiTypes]++;
            return;
        }
    }
    run.vertexFormat = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uintptr_t*>(re + 0x48) + kMeshVertexFormat);
    ObjectInputs(obj, run.objFlags, run.objC0, run.objF8, run.obj84, run.obj8A);
    run.ok = true;
}

// One item of a run drawn by us. False: not ours (nothing done, the caller gives it to the original).
bool OurItem(uintptr_t r, uintptr_t item, const ZRun& run, ListFn original, int index, uint32_t filter)
{
    const uintptr_t re = *reinterpret_cast<const uintptr_t*>(item + 8), obj = *reinterpret_cast<const uintptr_t*>(item + 0x18);
    g_zOurPi.clear();
    if (!IsMeshRe(re) || !obj)
        return g_zReject[kRejNotMesh]++, false;
    const uintptr_t mesh = *reinterpret_cast<const uintptr_t*>(re + 0x48);
    if (!mesh || *reinterpret_cast<const int*>(mesh + kMeshPrimitive) != 0)
        return g_zReject[kRejNotMesh]++, false;
    if (*reinterpret_cast<const uint32_t*>(mesh + kMeshVertexFormat) != run.vertexFormat)
        return g_zReject[kRejFormat]++, false;
    uint32_t flags, c0, f8, o84;
    uint8_t o8a;
    ObjectInputs(obj, flags, c0, f8, o84, o8a);
    if (flags != run.objFlags || c0 != run.objC0 || f8 != run.objF8 || o84 != run.obj84 || o8a != run.obj8A)
        return g_zReject[kRejObject]++, false;
    const int16_t od = *reinterpret_cast<const int16_t*>(obj + 0xC);
    if (od >= 0 && *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(kObjDataArrayPtr) + static_cast<uintptr_t>(od) * 0x20 + 0x10))
        return g_zReject[kRejInstances]++, false;
    if (*reinterpret_cast<const int*>(0x382E994C) && reinterpret_cast<ReFn0>(Vt(re, 8))(re) & 0xFF)
        return g_zReject[kRejSkinned]++, false;

    CycleScope cycles(kCyOurItem);
    // FX_ProcessBatchesList's per-item part: the object change (false: the original skips the item too), then prepare.
    if (!reinterpret_cast<ObjChangeFn>(kObjectChangeFn)(r, run.shader, run.resources, obj, re))
    {
        g_zSkipped++;
        return true;
    }
    // A new object starts a new batch in FX_ProcessBatchesList: FUN_380A1360 resets the masks and counts and takes
    // the object's flags (+0xAEC), light mask (+0xF1C), the shader's vertex format (+0x180C) and the matrix (+0xA80).
    const uint32_t technique = (run.sortKey & 0x3F) == 0x3F ? 0xFFFFFFFFu : (run.sortKey & 0x3F);
    reinterpret_cast<BatchStartFn>(kBatchStartFn)(r, run.shader, technique, run.resources, re);
    reinterpret_cast<ReFn0>(Vt(re, 2))(re);
    if (g_zownFlushDraw)
    {
        // Bisect experiment: the original batch flush after our object change and prepare.
        reinterpret_cast<FlushFn>(kFlushShaderFn)();
        g_zOurs++;
        return true;
    }
    if (*reinterpret_cast<const uintptr_t*>(r + kRdRendElement) != re || *reinterpret_cast<const int*>(r + kRdChunkStream) >= 0)
    {
        // Prepare chose another path (no element, or the chunk stream): the original's flush handles it.
        g_zReject[kRejPrepare]++;
        original(r, index, index + 1, filter);
        return true;
    }
    // What the VS vf7(0) of DrawShader_BatchStatic writes before the mesh: the vertex format the instance wants
    // (+0x180C, vf19's argument) and its stream masks (+0xB00/+0xB04, for the declaration).
    if (const uintptr_t vsInst = *reinterpret_cast<const uintptr_t*>(run.stageShader[0] + 0x70))
    {
        *reinterpret_cast<uint32_t*>(r + 0x180C) = *reinterpret_cast<const uint8_t*>(vsInst + 0x8B);
        *reinterpret_cast<uint32_t*>(r + 0xB00) = *reinterpret_cast<const uint16_t*>(vsInst + 0x84);
        *reinterpret_cast<uint32_t*>(r + 0xB04) = *reinterpret_cast<const uint16_t*>(vsInst + 0x82);
    }
    // DrawShader_BatchStatic's per-mesh part, then the object.
    reinterpret_cast<ReFn2>(Vt(re, 19))(re, *reinterpret_cast<const uint32_t*>(r + 0x180C), *reinterpret_cast<const uint32_t*>(r + 0xB04));
    *reinterpret_cast<int*>(r + 0xCB8) += 1;
    const uint32_t restore = reinterpret_cast<DeclSetupFn>(kDeclSetupFn)(r);
    // Streams: our own from the mesh into the packet (the engine's binding caches stay as they are, the back end
    // restores the bindings after the draw), else vf17.
    PacketGeometry geo;
    const bool ownStreams = !g_zownEngineDraw && ExtractStreams(r, re, geo);
    if (reinterpret_cast<DeclSetFn>(kDeclSetFn)(r, *reinterpret_cast<const uint32_t*>(r + 0xB00), *reinterpret_cast<const uint32_t*>(r + 0x180C)) >= 0 &&
        (ownStreams || reinterpret_cast<ReFn1>(Vt(re, 17))(re, run.pass) & 0xFF))
    {
        if (ownStreams)
        {
            *reinterpret_cast<int*>(r + kRdIndexBase) = static_cast<int>(geo.indexBase); // what vf17 leaves
            g_zOwnStreams++;
        }
        *reinterpret_cast<int*>(r + 0xB44) += 1;
        *reinterpret_cast<uintptr_t*>(r + kRdCurObject) = obj;
        *reinterpret_cast<uintptr_t*>(r + kRdObjMatrix) = obj + 0x14;
        for (const uintptr_t s : run.stageShader)
        {
            Expect group[kMaxGroupParams];
            int stage = 0, cbVectors = 0;
            const int n = s ? ComputePiGroup(s, group, stage, cbVectors) : 0;
            for (int i = 0; i < n && g_zownVerify; i++)
                g_zOurPi.push_back({group[i].reg, group[i].slot, stage, group[i].floats, {}}), memcpy(g_zOurPi.back().v, group[i].v, sizeof(group[i].v));
            if (n > 0 && (g_zownEngineDraw || !HoldGroup(stage, group, n, cbVectors)))
                for (int i = 0; i < n; i++)
                    g_cbWrite(group[i].reg, group[i].slot, stage, group[i].v, group[i].floats, cbVectors);
        }
        if (g_zownEngineDraw)
            reinterpret_cast<ReFn2>(Vt(re, 15))(re, run.loopArg, run.pass); // the engine's draw (experiment)
        else if (ownStreams)
        {
            geo.skipTargets = true; // the run's render targets are set; our items push none
            geo.skipCommit = !CommitNeeded(r, obj);
            g_zCommitSkipped += geo.skipCommit;
            PacketDraw(r, &geo);
        }
        else
            PacketDraw(r);
    }
    if (restore)
    {
        struct
        {
            uint64_t a;
            uint32_t b, c;
        } saved{*reinterpret_cast<const uint64_t*>(r + 0x28808), *reinterpret_cast<const uint32_t*>(r + 0x28810), *reinterpret_cast<const uint32_t*>(r + 0x28814)};
        reinterpret_cast<DeclRestoreFn>(kDeclRestoreFn)(r, &saved);
    }
    *reinterpret_cast<uint32_t*>(r + 0xB18) &= 0xFF;
    g_zOurs++;
    if (g_zownVerify)
    {
        // The original on the same item: it must not need any state ours left out, and its constants must be ours
        // (per instance) or the base group's (everything else, which we did not write again).
        std::vector<uint32_t> ops;
        std::vector<CbWrite> writes;
        g_cbCapture = &writes;
        D3DLayerTrace(&ops);
        original(r, index, index + 1, filter);
        D3DLayerTrace(nullptr);
        g_cbCapture = nullptr;
        g_zVerified++;
        if (ownStreams)
        {
            // vf17 left its streams in the engine's caches: they must be ours.
            g_zStreamsCompared++;
            bool same = *reinterpret_cast<const uintptr_t*>(r + 0xCA0) == geo.index.buffer &&
                        *reinterpret_cast<const int*>(r + kRdIndexBase) == static_cast<int>(geo.indexBase);
            for (uint32_t i = 0; i < geo.streams && same; i++)
                same = *reinterpret_cast<const uintptr_t*>(r + 0xB60 + geo.s[i].slot * 0x10) == geo.s[i].buffer &&
                       *reinterpret_cast<const uint32_t*>(r + 0xB68 + geo.s[i].slot * 0x10) == geo.s[i].offset;
            g_zStreamsMismatch += !same;
        }
        const auto same = [](const CbWrite& a, const CbWrite& b) {
            return a.floats == b.floats && memcmp(a.v, b.v, static_cast<size_t>(a.floats) * sizeof(float)) == 0;
        };
        const auto logMismatch = [](const char* what, const CbWrite& orig, const CbWrite& expected) {
            if (g_zMismatchLog.size() > 6000)
                return;
            char line[600];
            int len = snprintf(line, sizeof(line), "  %s stage %d slot %d reg %d floats %d\n    orig:", what, orig.stage, orig.slot, orig.reg, orig.floats);
            for (int k = 0; k < orig.floats && k < 16 && len < 280; k++)
                len += snprintf(line + len, sizeof(line) - len, " %g", orig.v[k]);
            len += snprintf(line + len, sizeof(line) - len, "\n    ours:");
            for (int k = 0; k < expected.floats && k < 16 && len < 580; k++)
                len += snprintf(line + len, sizeof(line) - len, " %g", expected.v[k]);
            g_zMismatchLog += line;
            g_zMismatchLog += "\n";
        };
        for (const CbWrite& w : writes)
        {
            const CbWrite* ours = nullptr;
            for (const CbWrite& o : g_zOurPi)
                if (o.stage == w.stage && o.slot == w.slot && o.reg == w.reg)
                    ours = &o;
            if (ours)
            {
                g_zPiCompared++;
                if (!same(w, *ours))
                    g_zPiMismatch++, logMismatch("PI", w, *ours);
                continue;
            }
            const CbWrite* base = nullptr;
            for (const CbWrite& b : g_zBaseWrites)
                if (b.stage == w.stage && b.slot == w.slot && b.reg == w.reg)
                    base = &b; // the last one
            g_zPbCompared++;
            if (!base)
                g_zPbMissing++;
            else if (!same(w, *base))
                g_zPbMismatch++, logMismatch("other", w, *base);
        }
        // Our per-instance values the original did not write at all.
        bool missing = false;
        for (const CbWrite& o : g_zOurPi)
        {
            bool found = false;
            for (const CbWrite& w : writes)
                found = found || (w.stage == o.stage && w.slot == o.slot && w.reg == o.reg);
            missing = missing || !found;
        }
        if (missing)
        {
            g_zPiNotWritten++;
            if (g_zNotWrittenLog.size() < 4000)
            {
                std::string s = "  original wrote no PI: shader " + ShaderName(run.shader) + ", ours";
                for (const CbWrite& o : g_zOurPi)
                    s += " [st " + std::to_string(o.stage) + " slot " + std::to_string(o.slot) + " reg " + std::to_string(o.reg) + "]";
                s += ", its writes";
                for (const CbWrite& w : writes)
                    s += " [st " + std::to_string(w.stage) + " slot " + std::to_string(w.slot) + " reg " + std::to_string(w.reg) + " n " + std::to_string(w.floats) + "]";
                s += ", ops";
                for (const uint32_t op : ops)
                    s += std::string(" ") + D3DLayerOpName(op & 0xFFFF);
                g_zNotWrittenLog += s + "\n";
            }
        }
        bool drew = false;
        for (const uint32_t op : ops)
            drew = drew || strncmp(D3DLayerOpName(op & 0xFFFF), "DrawIndexed", 11) == 0 || strncmp(D3DLayerOpName(op & 0xFFFF), "Draw", 4) == 0;
        if (!drew)
        {
            g_zOrigNoDraw++;
            if (g_zNoDrawLog.size() < 3000)
            {
                char line[300];
                const uintptr_t m = *reinterpret_cast<const uintptr_t*>(re + 0x48);
                snprintf(line, sizeof(line), "  no draw by the original: shader %s, obj flags 0x%x +0xC0 0x%x +0x84 0x%x opacity %g, mesh vf %u prim %d, pi %zu, ops %zu\n",
                         ShaderName(run.shader).c_str(), flags, c0, o84, *reinterpret_cast<const float*>(obj + 0xF8),
                         *reinterpret_cast<const uint32_t*>(m + kMeshVertexFormat), *reinterpret_cast<const int*>(m + kMeshPrimitive), g_zOurPi.size(), ops.size());
                g_zNoDrawLog += line;
            }
        }
        for (const uint32_t op : ops)
        {
            const char* name = D3DLayerOpName(op & 0xFFFF);
            // Streams are not expected either: ours set them through the same vf17, so the engine's cache must skip them.
            static const char* const kExpected[] = {"VSSetConstantBuffers", "PSSetConstantBuffers", "GSSetConstantBuffers", "DrawIndexed", "Upload",
                                                    "IASetPrimitiveTopology"};
            bool expected = false;
            for (const char* e : kExpected)
                expected = expected || strcmp(name, e) == 0;
            if (!expected)
                g_zVerifyOps[name]++;
        }
    }
    return true;
}

bool g_zownToggle;     // CRYMT_ZOWN=toggle: our items only on odd frames (toggle2: even), for same-run image comparisons
// CRYMT_ZOWNSPLIT=0: groups of items sharing a render element stay whole with the original, which draws them as one
// batch (one flush, ~700 cycles per further object on harbor), cheaper than our item path (~1100 cycles); only items with
// a render element of their own (a full flush each, ~3700 cycles) are ours.
bool g_zownSplit = true;
bool g_zownToggleEven;
uint64_t g_zGroupsTaken, g_zGroupsInstanced;
// What the original did with the groups ZListHook gave it: [predicted instanced][plain loops ran][instanced loops ran]:
// calls, items, objects drawn by plain loops.
uint64_t g_zBaseOutcome[2][2][2][3];

// Whether the flush would draw a batch of n merged objects with hardware instancing (FUN_3802D6D0's test: the
// instancing cvar, more objects than the threshold, a shader that supports it, no shader debug mode).
bool PredictInstanced(uint32_t key, int n)
{
    uintptr_t shader, resources;
    KeyShader(key, shader, resources);
    return *reinterpret_cast<const int*>(0x382E9A64) != 0 && static_cast<uint32_t>(n) > *reinterpret_cast<const uint32_t*>(0x382E9B64) && shader &&
           (*reinterpret_cast<const uint32_t*>(shader + 0x3C) & 0x6000000) != 0 && *reinterpret_cast<const int*>(kShaderDebugMode) == 0;
}

// Draw groups (CRYMT_ZGROUPS=1 with CRYMT_ZOWN and CRYMT_ZPACKETS): a group of consecutive z-prepass items sharing a
// render element that the engine would batch without instancing. The engine draws the first item, which sets the
// shaders, states, streams, index range and per-batch constants. For the others the render thread only checks them and
// takes ~16 bytes each; one stream command carries the group, and the stream thread computes every object's per-instance
// constants (the PiShared/PiObject functions above) and executes one draw packet per object. The engine's per-object
// counter (+0xB44, BendInfo's cache key), current object and statistics are advanced as its loop would.
// CRYMT_ZGROUPVERIFY=1: the render thread also computes the values (the stream thread compares them), and the engine
// then draws the same objects again (harmless in a depth pass) with its constant writes captured and compared in order.
struct GroupStage
{
    uint8_t stage, slot;
    uint16_t cbVectors;
    uint32_t count;  // SCGParam entries
    uint64_t params; // the instance's per-instance parameter array
};
struct GroupHeader
{
    uint32_t indexCount, start;
    uint16_t objects;
    uint8_t stages, verify;
    uint8_t values, pad[3]; // values: the payload carries the computed constants instead of the object pointers
    GroupStage st[3];
    PiShared shared;
};
bool g_zGroups, g_zGroupVerify, g_zGroupsAll; // all: also groups the engine would instance (drawn one by one by us)
// The payload carries the object pointers and the stream thread computes the values (CRYMT_ZGROUPVERIFY compares them
// with the render thread's: 0 of 7409 differed on mine). CRYMT_ZGROUPVALUES=1: the render thread computes them and the
// payload carries the values (safe even if the engine refilled the objects before the stream thread runs; costs it
// ResolveStage per object).
bool g_zGroupDefer;
uint64_t g_zgGroups, g_zgObjects, g_zgReject[8], g_zgCut, g_zgVerifyCompared, g_zgVerifyMismatch, g_zgVerifyCount;
uint64_t g_zgInstanceDiff, g_zgBatchCompared, g_zgBatchDiff, g_zgBatchMissing;
// CRYMT_ZGROUPRELAX=1: objects that differ from the batch's first only in m_nRAEId/m_nTextureID (+0xCC/+0xCE, the
// engine's main reason not to merge in harbor's z-prepass) join the batch when nothing of the batch reads them: the PS
// resources (FUN_380477B0) read +0xCC for the placeholder texture 0x38643CD0 and +0xCE for a texture whose kind (+0xF8)
// is 5, and the RT mask (FUN_38029090) tests +0xCE > 0 under batch flag 0x1000. The other merge fields stay.
bool g_zGroupRelax;
uint64_t g_zgRelaxBatches, g_zgRelaxed, g_zgRelaxBlocked;
bool PsReadsObjectTextures(uintptr_t psShader, uintptr_t res)
{
    const uintptr_t inst = psShader ? *reinterpret_cast<const uintptr_t*>(psShader + 0x70) : 0;
    if (!inst)
        return true;
    const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(inst + 0x48);
    if (!holder || !*reinterpret_cast<const uintptr_t*>(holder))
        return false;
    const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(holder);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(entries - 4);
    for (uint32_t i = 0; i < count; i++)
    {
        uintptr_t tex = *reinterpret_cast<const uintptr_t*>(entries + static_cast<uintptr_t>(i) * 0x40 + 0x10);
        if (!tex)
            continue;
        if (tex >= 0x38641F90 && tex < 0x38643E71)
        {
            if (tex >= 0x386437F0)
                return true; // the object placeholder (0x38643CD0) or another engine texture: be safe
            // A material slot: the resources' texture, as FUN_380477B0 resolves it (defaults are not object textures).
            const uintptr_t slot = res ? *reinterpret_cast<const uintptr_t*>(res + 0x48 + (tex - 0x38641F90) / 0x1A0 * 8) : 0;
            tex = slot ? *reinterpret_cast<const uintptr_t*>(slot + 0x120) : 0;
            if (!tex)
                continue;
        }
        if (*reinterpret_cast<const int*>(tex + 0xF8) == 5)
            return true;
    }
    return false;
}

// Why the next object did not merge, by the first field that differs (CRenderObject, Mod SDK names).
uint64_t g_zgMergeFail[8];
const char* const kMergeFieldNames[8] = {"flags (m_ObjFlags & 0x47FF0200)", "m_DynLMMask +0x8C", "m_nMaterialLayers +0x84", "m_nRAEId/m_nTextureID +0xCC",
                                         "m_pShadowCasters +0xE8", "m_pCharInstance +0x110", "objdata instance matrices", "no object"};
void NoteMergeFail(uintptr_t base, uintptr_t obj)
{
    const auto u32 = [](uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); };
    const auto u64 = [](uintptr_t a) { return *reinterpret_cast<const uint64_t*>(a); };
    const int why = !obj ? 7 : ((u32(obj + 0x10) ^ u32(base + 0x10)) & 0x47FF0200) ? 0 : u32(obj + 0x8C) != u32(base + 0x8C) ? 1
                  : u32(obj + 0x84) != u32(base + 0x84) ? 2 : u32(obj + 0xCC) != u32(base + 0xCC) ? 3 : u64(obj + 0xE8) != u64(base + 0xE8) ? 4
                  : u64(obj + 0x110) != u64(base + 0x110) ? 5 : 6;
    g_zgMergeFail[why]++;
}

// FUN_380A11F0's test whether the engine merges an object into the current batch (same CREMesh; object flags under
// 0x47FF0000, +0x8C, +0x84, +0xCC, +0xE8, +0x110 equal), plus the same flag bit 0x200 (a merge ORs it into the batch
// flags) and no objdata instance matrices: then the engine draws it with the batch's state, as our group does.
// relaxTexture: m_nRAEId/m_nTextureID (+0xCC/+0xCE) may differ (the caller checked that the batch does not use them).
bool MergesWith(uintptr_t base, uintptr_t obj, bool relaxTexture)
{
    const auto u32 = [](uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); };
    const auto u64 = [](uintptr_t a) { return *reinterpret_cast<const uint64_t*>(a); };
    if (((u32(obj + 0x10) ^ u32(base + 0x10)) & 0x47FF0200) != 0 || u32(obj + 0x8C) != u32(base + 0x8C) || u32(obj + 0x84) != u32(base + 0x84) ||
        (!relaxTexture && u32(obj + 0xCC) != u32(base + 0xCC)) || u64(obj + 0xE8) != u64(base + 0xE8) || u64(obj + 0x110) != u64(base + 0x110))
        return false;
    const int16_t od = *reinterpret_cast<const int16_t*>(obj + 0xC);
    return !(od >= 0 && *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(kObjDataArrayPtr) + static_cast<uintptr_t>(od) * 0x20 + 0x10));
}
std::atomic<uint64_t> g_zgDrawn, g_zgResolveFailed, g_zgRaceCompared, g_zgRaceMismatch;
const char* const kGroupRejectNames[8] = {"base not one plain loop", "draw disabled or instances not ready", "not a triangle list",
                                          "per-instance group not ours", "packet too big", "next object does not merge (new base)",
                                          "first object not computable", ""};

// The written parameters of a stage's per-instance group, in the original's order, into dst (cbVectors * 4 floats).
bool ResolveStage(const GroupStage& st, const PiShared& s, const PiObject& o, float* dst)
{
    memset(dst, 0, static_cast<size_t>(st.cbVectors) * 16);
    for (uint32_t i = 0; i < st.count; i++)
    {
        const uintptr_t p = st.params + static_cast<uintptr_t>(i) * kParamSize;
        const int16_t reg = *reinterpret_cast<const int16_t*>(p + 0xC);
        if (!((*reinterpret_cast<const uint32_t*>(p + 8) & 8) || reg >= 0))
            continue;
        const int floats = *reinterpret_cast<const int32_t*>(p + 0x10);
        float v[16];
        if (!OwnPiValue(static_cast<int>(*reinterpret_cast<const uint32_t*>(p + 0x18) & 0xFF), floats, s, o, v))
            return false;
        memcpy(dst + reg, v, static_cast<size_t>(floats) * sizeof(float));
    }
    return true;
}

// The values of every stage of the first count objects, in payload order, computed now; returns how many objects were
// computable (a value may not be, e.g. the night vision parameter not looked up yet).
size_t GroupValues(const GroupHeader& h, const std::vector<PiObject>& objects, size_t count, std::vector<float>& values)
{
    uint32_t floats = 0;
    for (uint8_t s = 0; s < h.stages; s++)
        floats += h.st[s].cbVectors * 4u;
    values.resize(count * floats);
    float* e = values.data();
    for (size_t m = 0; m < count; m++)
        for (uint8_t s = 0; s < h.stages; s++)
        {
            if (!ResolveStage(h.st[s], h.shared, objects[m], e))
                return m;
            e += h.st[s].cbVectors * 4;
        }
    return count;
}

// The group command: header, then the values (or with CRYMT_ZGROUPDEFER the object pointers, and the values to compare
// with under CRYMT_ZGROUPVERIFY).
void SendGroup(GroupHeader& h, const std::vector<PiObject>& objects, const std::vector<float>& values, bool verify)
{
    h.objects = static_cast<uint16_t>(objects.size());
    h.values = g_zGroupDefer ? 0 : 1;
    h.verify = g_zGroupDefer && verify ? 1 : 0;
    const size_t objectBytes = h.values ? 0 : objects.size() * sizeof(PiObject);
    const size_t valueBytes = h.values || h.verify ? values.size() * sizeof(float) : 0;
    static std::vector<uint8_t> payload;
    payload.resize(sizeof(h) + objectBytes + valueBytes);
    memcpy(payload.data(), &h, sizeof(h));
    if (objectBytes)
        memcpy(payload.data() + sizeof(h), objects.data(), objectBytes);
    if (valueBytes)
        memcpy(payload.data() + sizeof(h) + objectBytes, values.data(), valueBytes);
    D3DLayerDrawGroup(payload.data(), static_cast<uint32_t>(payload.size()));
}

// A stage's per-instance group as a GroupStage; false when we cannot take it (unknown types, several slots, ...).
bool DescribeStage(uintptr_t shader, GroupStage& st, bool& present)
{
    present = false;
    const uintptr_t inst = *reinterpret_cast<const uintptr_t*>(shader + 0x70);
    const auto* range = inst ? *reinterpret_cast<const uintptr_t* const*>(inst + 0x38 + 8) : nullptr;
    if (!range || range[1] <= range[0])
        return true;
    const int stage = *reinterpret_cast<const int*>(shader + 0x20);
    const int cbVectors = *reinterpret_cast<const int*>(inst + 0x70 + 4);
    if (stage < 0 || stage > 2 || cbVectors <= 0 || cbVectors > 64)
        return false;
    st = {static_cast<uint8_t>(stage), 0, static_cast<uint16_t>(cbVectors), static_cast<uint32_t>((range[1] - range[0]) / kParamSize), range[0]};
    int slot = -1;
    for (uint32_t i = 0; i < st.count; i++)
    {
        const uintptr_t p = range[0] + static_cast<uintptr_t>(i) * kParamSize;
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(p + 8);
        const int16_t reg = *reinterpret_cast<const int16_t*>(p + 0xC);
        if (!((flags & 8) || reg >= 0))
            continue;
        const int type = static_cast<int>(*reinterpret_cast<const uint32_t*>(p + 0x18) & 0xFF);
        const int floats = *reinterpret_cast<const int32_t*>(p + 0x10);
        const int pslot = *reinterpret_cast<const int16_t*>(p + 0xE);
        const bool known = type == g_tObjMatrix ? floats == 12
                                                : (type == g_tBendInfo || type == g_tAlphaTest || type == g_tObjectAmbColComp || type == g_tAmbientOpacity) &&
                                                      floats >= 1 && floats <= 4;
        if (!(flags & 0x40000) || !known || reg < 0 || reg + floats > cbVectors * 4 || (slot >= 0 && pslot != slot))
            return false;
        slot = pslot;
    }
    if (slot < 0)
        return true; // nothing written
    st.slot = static_cast<uint8_t>(slot);
    present = true;
    return true;
}

// One batch of the group from first: the engine draws first, we draw the objects after it that the engine would merge
// into its batch. Returns where the next batch starts, or -1 when the rest must go to the engine.
int DrawGroupOnce(uintptr_t r, int first, int last, uint32_t filter, uintptr_t items, ListFn original)
{
    const auto at = [items](int i) { return items + static_cast<uintptr_t>(i) * kRenderItemSize; };
    g_lastPass = 0;
    g_objectLoops = g_instancedLoops = 0;
    std::vector<CbWrite> baseWrites;
    if (g_zGroupVerify)
        g_cbCapture = &baseWrites;
    {
        CycleScope cycles(kCyGroupBase);
        original(r, first, first + 1, filter);
    }
    g_cbCapture = nullptr;
    g_zItems++;
    CycleScope ownCycles(kCyGroupOwn);
    const auto reject = [&](int why) {
        g_zgReject[why]++;
        return -1;
    };
    ZRun run;
    BeginRun(r, at(first), g_objectLoops == 1 && g_instancedLoops == 0, run);
    if (!run.ok)
        return reject(0);
    const uintptr_t vs = *reinterpret_cast<const uintptr_t*>(kCurVsInstance), ps = *reinterpret_cast<const uintptr_t*>(kCurPsInstance),
                    gs = *reinterpret_cast<const uintptr_t*>(kCurGsInstance);
    if (*reinterpret_cast<const int*>(kDrawDisabled) || !vs || !ps || *reinterpret_cast<const char*>(vs + kInstanceNotReady) ||
        *reinterpret_cast<const char*>(ps + kInstanceNotReady) || (gs && *reinterpret_cast<const char*>(gs + kInstanceNotReady)))
        return reject(1);
    const int indexCount = *reinterpret_cast<const int*>(r + kRdIndexCount);
    if (*reinterpret_cast<const int*>(r + kRdTopology) != 4 || *reinterpret_cast<const int*>(r + kRdChunkStream) >= 0 || indexCount / 3 == 0)
        return reject(2);
    GroupHeader h{};
    h.indexCount = static_cast<uint32_t>(indexCount);
    h.start = static_cast<uint32_t>(*reinterpret_cast<const int*>(r + kRdIndexBase) + *reinterpret_cast<const int*>(r + kRdFirstIndex));
    uint32_t packetBytes = sizeof(DrawPacketHeader);
    for (const uintptr_t sh : run.stageShader)
    {
        bool present = false;
        if (sh && !DescribeStage(sh, h.st[h.stages], present))
            return reject(3);
        if (present)
            packetBytes += sizeof(DrawPacketConstants) + h.st[h.stages++].cbVectors * 16u;
    }
    if (packetBytes > kDrawPacketMaxBytes)
        return reject(4);
    CaptureShared(r, h.shared);
    h.verify = g_zGroupVerify ? 1 : 0;
    // Objects that match the base (the engine would batch them the same way); a different one ends our part.
    static std::vector<PiObject> objects;
    objects.clear();
    const uintptr_t baseObj = *reinterpret_cast<const uintptr_t*>(at(first) + 0x18);
    const bool relax = g_zGroupRelax && !PsReadsObjectTextures(run.stageShader[1], h.shared.res);
    g_zgRelaxBatches += relax;
    const bool flag1000 = (h.shared.flagsB & 0x1000) != 0;
    const bool baseCe = *reinterpret_cast<const int16_t*>(baseObj + 0xCE) > 0;
    for (int j = first + 1; j < last; j++)
    {
        const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(at(j) + 0x18);
        bool ok = obj && MergesWith(baseObj, obj);
        if (obj && !ok && relax && MergesWith(baseObj, obj, true))
        {
            ok = !flag1000 || (*reinterpret_cast<const int16_t*>(obj + 0xCE) > 0) == baseCe;
            g_zgRelaxed += ok;
            g_zgRelaxBlocked += !ok;
        }
        if (!ok)
        {
            NoteMergeFail(baseObj, obj);
            break;
        }
        objects.push_back({obj, obj + 0x14});
    }
    if (objects.empty())
    {
        g_zgReject[5]++;
        return first + 1;
    }
    // Every value must be computable (e.g. the night vision parameter looked up): all of them when they are sent or
    // compared, one that is not ending our part; else the first object as a probe.
    static std::vector<float> values;
    const size_t need = g_zGroupDefer && !g_zGroupVerify ? 1 : objects.size();
    const size_t computable = GroupValues(h, objects, need, values);
    if (computable == 0)
        return reject(6);
    if (computable < need)
    {
        objects.resize(computable);
        uint32_t floats = 0;
        for (uint8_t s = 0; s < h.stages; s++)
            floats += h.st[s].cbVectors * 4u;
        values.resize(computable * floats);
    }
    SendGroup(h, objects, values, g_zGroupVerify);
    // What the engine's per-object loop leaves (FUN_380268D0 0x380278A3): +0xB44 advanced per object and once more when
    // it puts back the batch's first object as the current one (+0xA78, matrix +0xA80), which the base's draw left set:
    // they stay. Leaving the group's last object there broke FX_ObjectChange's early out (object == +0xAB8, the base):
    // the base object's next render element (the Metal case's trims and latch) was drawn with the other object's matrix.
    const int n = static_cast<int>(objects.size());
    *reinterpret_cast<int*>(r + 0xB44) += n + 1;
    const int list = *reinterpret_cast<const int*>(r + kRdStatList);
    *reinterpret_cast<int*>(r + kRdStatTriangles + static_cast<uintptr_t>(list) * 4) += n * (indexCount / 3);
    *reinterpret_cast<int*>(r + kRdStatDraws + static_cast<uintptr_t>(list) * 4) += n;
    g_zgGroups++;
    g_zgObjects += static_cast<uint64_t>(n);
    g_zOurs += static_cast<uint64_t>(n);
    g_zItems += static_cast<uint64_t>(n);
    if (g_zGroupVerify)
    {
        // The engine on each of these objects alone (a second depth draw is harmless): it must pick the base's shader
        // instances, write our per-instance values, and write the per-batch constants the base left.
        alignas(16) float mine[64 * 4];
        for (int m = 0; m < n; m++)
        {
            std::vector<CbWrite> writes;
            g_cbCapture = &writes;
            original(r, first + 1 + m, first + 2 + m, filter);
            g_cbCapture = nullptr;
            g_zgInstanceDiff += *reinterpret_cast<const uintptr_t*>(kCurVsInstance) != vs || *reinterpret_cast<const uintptr_t*>(kCurPsInstance) != ps;
            for (const CbWrite& wr : writes)
            {
                const GroupStage* st = nullptr;
                for (uint8_t s = 0; s < h.stages; s++)
                    if (wr.stage == h.st[s].stage && wr.slot == h.st[s].slot)
                        st = &h.st[s];
                if (st)
                {
                    ResolveStage(*st, h.shared, objects[static_cast<size_t>(m)], mine);
                    g_zgVerifyCompared++;
                    if (wr.reg < 0 || wr.reg + wr.floats > st->cbVectors * 4 || memcmp(wr.v, mine + wr.reg, static_cast<size_t>(wr.floats) * sizeof(float)) != 0)
                        g_zgVerifyMismatch++;
                    continue;
                }
                const CbWrite* b = nullptr;
                for (const CbWrite& bw : baseWrites)
                    if (bw.stage == wr.stage && bw.slot == wr.slot && bw.reg == wr.reg)
                        b = &bw;
                g_zgBatchCompared++;
                if (!b)
                    g_zgBatchMissing++;
                else if (b->floats != wr.floats || memcmp(b->v, wr.v, static_cast<size_t>(wr.floats) * sizeof(float)) != 0)
                    g_zgBatchDiff++;
            }
        }
    }
    if (first + 1 + n < last)
        g_zgCut++;
    return first + 1 + n;
}

// The group [first, last) of the list (items sharing a render element), batch by batch as the engine would split it.
void DrawGroup(uintptr_t r, int first, int last, uint32_t filter, uintptr_t items, ListFn original)
{
    for (int i = first; i < last;)
    {
        const int next = DrawGroupOnce(r, i, last, filter, items, original);
        if (next < 0)
        {
            CycleScope restCycles(kCyGroupRest);
            original(r, i + 1, last, filter);
            g_zItems += static_cast<uint64_t>(last - i - 1);
            return;
        }
        i = next;
    }
}

// Loop groups (CRYMT_LOOPGROUPS=1 with CRYMT_ZPACKETS, D3D12 + stream): a batch the engine merged (its object list at
// renderer +0xAC0 holds the first object, the current one, and the merged ones) in any pass. The engine's loop draws the
// first object (list count 0 for that call, its single-object case), which binds the pass's shaders, states, streams and
// per-batch constants; the merged objects go as one group command, as in DrawGroupOnce. The loop runs once per pass of
// the technique, and so does this. CRYMT_LOOPGROUPVERIFY=1: the engine's loop then draws the merged objects again (list
// without the first), its per-instance constant writes compared with ours in order.
uint64_t g_lgBatches, g_lgObjects, g_lgReject[8], g_lgCompared, g_lgMismatch, g_lgMissing, g_lgOther;
std::string g_lgLog; // first mismatches
std::unordered_map<std::string, uint64_t> g_lgByType; // mismatches and missing values by parameter type, level, list
const char* const kLoopRejectNames[8] = {"first object not the current", "not a plain CREMesh", "objdata instance matrices",
                                         "per-instance group not ours", "packet too big", "draw state after the first",
                                         "first merged object not computable", ""};

bool LoopGroup(uintptr_t r, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    auto* list = reinterpret_cast<uint8_t*>(r + kRdBatchObjects);
    auto* objs = *reinterpret_cast<uintptr_t**>(list);
    const int count = *reinterpret_cast<int*>(list + 8);
    if (count < 2 || !objs || !b || !PacketsReady())
        return false;
    ResolveTypes();
    if (objs[0] != *reinterpret_cast<const uintptr_t*>(r + kRdCurObject))
        return g_lgReject[0]++, false;
    const uintptr_t re = *reinterpret_cast<const uintptr_t*>(r + kRdRendElement);
    const uintptr_t mesh = re && IsMeshRe(re) ? *reinterpret_cast<const uintptr_t*>(re + 0x48) : 0;
    if (!mesh || *reinterpret_cast<const int*>(mesh + kMeshPrimitive) != 0 ||
        (*reinterpret_cast<const int*>(0x382E994C) && reinterpret_cast<uint64_t(__fastcall*)(uintptr_t)>(Vt(re, 8))(re) & 0xFF))
        return g_lgReject[1]++, false;
    for (int i = 1; i < count; i++)
    {
        const int16_t od = *reinterpret_cast<const int16_t*>(objs[i] + 0xC);
        if (!objs[i] || (od >= 0 && *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(kObjDataArrayPtr) + static_cast<uintptr_t>(od) * 0x20 + 0x10)))
            return g_lgReject[2]++, false;
    }
    // The engine draws the first object; then the merged ones by list (the engine's own way) if we cannot take them.
    const auto engineRest = [&] {
        *reinterpret_cast<uintptr_t**>(list) = objs + 1;
        *reinterpret_cast<int*>(list + 8) = count - 1;
        g_drawObjects(r, a, b, c, d);
        *reinterpret_cast<uintptr_t**>(list) = objs;
        *reinterpret_cast<int*>(list + 8) = count;
    };
    *reinterpret_cast<int*>(list + 8) = 0;
    g_drawObjects(r, a, b, c, d);
    *reinterpret_cast<int*>(list + 8) = count;
    const uintptr_t vs = *reinterpret_cast<const uintptr_t*>(kCurVsInstance), ps = *reinterpret_cast<const uintptr_t*>(kCurPsInstance),
                    gs = *reinterpret_cast<const uintptr_t*>(kCurGsInstance);
    const int indexCount = *reinterpret_cast<const int*>(r + kRdIndexCount);
    if (*reinterpret_cast<const int*>(kDrawDisabled) || !vs || !ps || *reinterpret_cast<const char*>(vs + kInstanceNotReady) ||
        *reinterpret_cast<const char*>(ps + kInstanceNotReady) || (gs && *reinterpret_cast<const char*>(gs + kInstanceNotReady)) ||
        *reinterpret_cast<const int*>(r + kRdTopology) != 4 || *reinterpret_cast<const int*>(r + kRdChunkStream) >= 0 || indexCount / 3 == 0)
    {
        g_lgReject[5]++;
        engineRest();
        return true;
    }
    // The stages' per-instance groups of the instances the loop just selected (vf7 runs inside the loop).
    GroupHeader h{};
    uint32_t packetBytes = sizeof(DrawPacketHeader);
    for (const uintptr_t off : {uintptr_t{8}, uintptr_t{0x10}, uintptr_t{0x18}})
    {
        const uintptr_t sh = *reinterpret_cast<const uintptr_t*>(b + off);
        bool present = false;
        if (sh && !DescribeStage(sh, h.st[h.stages], present))
        {
            g_lgReject[3]++;
            engineRest();
            return true;
        }
        if (present)
            packetBytes += sizeof(DrawPacketConstants) + h.st[h.stages++].cbVectors * 16u;
    }
    if (packetBytes > kDrawPacketMaxBytes)
    {
        g_lgReject[4]++;
        engineRest();
        return true;
    }
    h.indexCount = static_cast<uint32_t>(indexCount);
    h.start = static_cast<uint32_t>(*reinterpret_cast<const int*>(r + kRdIndexBase) + *reinterpret_cast<const int*>(r + kRdFirstIndex));
    CaptureShared(r, h.shared);
    h.verify = 0;
    const int n = count - 1;
    static std::vector<PiObject> objects;
    objects.clear();
    for (int i = 1; i < count; i++)
        objects.push_back({objs[i], objs[i] + 0x14});
    static std::vector<float> values;
    const size_t need = g_zGroupDefer ? 1 : objects.size();
    if (GroupValues(h, objects, need, values) < need)
    {
        g_lgReject[6]++;
        engineRest();
        return true;
    }
    SendGroup(h, objects, values, false);
    *reinterpret_cast<int*>(r + 0xB44) += n + 1; // the current object stays the first, as after the engine's loop
    const int statList = *reinterpret_cast<const int*>(r + kRdStatList);
    *reinterpret_cast<int*>(r + kRdStatTriangles + static_cast<uintptr_t>(statList) * 4) += n * (indexCount / 3);
    *reinterpret_cast<int*>(r + kRdStatDraws + static_cast<uintptr_t>(statList) * 4) += n;
    g_lgBatches++;
    g_lgObjects += static_cast<uint64_t>(n);
    if (g_loopGroupVerify)
    {
        std::vector<CbWrite> writes;
        g_cbCapture = &writes;
        engineRest();
        g_cbCapture = nullptr;
        size_t w = 0;
        alignas(16) float mine[64 * 4];
        for (const PiObject& o : objects)
            for (uint8_t s = 0; s < h.stages; s++)
            {
                ResolveStage(h.st[s], h.shared, o, mine);
                for (uint32_t i = 0; i < h.st[s].count; i++)
                {
                    const uintptr_t p = h.st[s].params + static_cast<uintptr_t>(i) * kParamSize;
                    const int16_t reg = *reinterpret_cast<const int16_t*>(p + 0xC);
                    if (!((*reinterpret_cast<const uint32_t*>(p + 8) & 8) || reg >= 0))
                        continue;
                    const int floats = *reinterpret_cast<const int32_t*>(p + 0x10);
                    while (w < writes.size() && !(writes[w].stage == h.st[s].stage && writes[w].slot == h.st[s].slot && writes[w].reg == reg))
                    {
                        g_lgOther++;
                        w++;
                    }
                    g_lgCompared++;
                    const bool missing = w >= writes.size();
                    const bool differs = !missing && (writes[w].floats != floats || memcmp(writes[w].v, mine + reg, static_cast<size_t>(floats) * sizeof(float)) != 0);
                    if (missing || differs)
                    {
                        (missing ? g_lgMissing : g_lgMismatch)++;
                        char key[160];
                        snprintf(key, sizeof(key), "%s %s stage %d level %d list %d count %d", missing ? "missing" : "differs",
                                 TypeNames(*reinterpret_cast<const uint32_t*>(p + 0x18)).c_str(), h.st[s].stage, *reinterpret_cast<const int*>(kRecurseLevel),
                                 *reinterpret_cast<const int*>(r + kRdBatchListId), count);
                        g_lgByType[key]++;
                        if (differs && g_lgLog.size() < 5000)
                        {
                            char line[700];
                            int len = snprintf(line, sizeof(line), "  %s obj %llx flags %x shader %s\n    engine:", key, static_cast<unsigned long long>(o.obj),
                                               *reinterpret_cast<const uint32_t*>(o.obj + 0x10), ShaderName(*reinterpret_cast<const uintptr_t*>(r + kRdShader)).c_str());
                            for (int k = 0; k < floats && len < 400; k++)
                                len += snprintf(line + len, sizeof(line) - len, " %g", writes[w].v[k]);
                            len += snprintf(line + len, sizeof(line) - len, "\n    ours:  ");
                            for (int k = 0; k < floats && len < 680; k++)
                                len += snprintf(line + len, sizeof(line) - len, " %g", mine[reg + k]);
                            g_lgLog += line;
                            g_lgLog += "\n";
                        }
                        if (missing)
                            continue;
                    }
                    w++;
                }
            }
        g_lgOther += writes.size() - w;
    }
    return true;
}

// The stream thread's part: one draw packet per object with its computed per-instance constants.
void ExpandGroup(const uint8_t* payload)
{
    GroupHeader h;
    memcpy(&h, payload, sizeof(h));
    const auto* objects = reinterpret_cast<const PiObject*>(payload + sizeof(h));
    const auto* expect = reinterpret_cast<const float*>(payload + sizeof(h) + (h.values ? 0 : h.objects * sizeof(PiObject)));
    alignas(16) uint8_t packet[kDrawPacketMaxBytes];
    for (uint32_t m = 0; m < h.objects; m++)
    {
        DrawPacketHeader ph{h.indexCount, h.start, 0, h.stages, 0, 0, 0};
        uint32_t used = sizeof(ph);
        bool ok = true;
        for (uint8_t s = 0; s < h.stages; s++)
        {
            const GroupStage& st = h.st[s];
            const DrawPacketConstants c{st.stage, st.slot, st.cbVectors};
            memcpy(packet + used, &c, sizeof(c));
            auto* dst = reinterpret_cast<float*>(packet + used + sizeof(c));
            if (h.values)
            {
                memcpy(dst, expect, st.cbVectors * 16u);
                expect += st.cbVectors * 4;
            }
            else
            {
                ok = ResolveStage(st, h.shared, objects[m], dst) && ok;
                if (h.verify)
                {
                    g_zgRaceCompared++;
                    if (memcmp(dst, expect, st.cbVectors * 16u) != 0)
                        g_zgRaceMismatch++;
                    expect += st.cbVectors * 4;
                }
            }
            used += sizeof(c) + st.cbVectors * 16u;
        }
        memcpy(packet, &ph, sizeof(ph));
        if (ok)
        {
            Dx12DrawPacket(packet);
            g_zgDrawn++;
        }
        else
            g_zgResolveFailed++;
    }
}

// Bucketed own z-prepass (CRYMT_ZBUCKET=1 with CRYMT_ZOWN): a depth-only pass leaves the same depth for any draw order,
// so within a list range the items go by state key instead of by position. The first item of a key is drawn by the
// original, which leaves that key's shaders, per-batch constants, textures and states set; every other item of the key
// is ours right after it. Groups of items sharing a render element stay whole with the original (it batches them, maybe
// instanced), and items our path cannot take go to the original one by one (and become the base of what follows).
bool g_zBucket;
uint64_t g_zbBuckets, g_zbBases, g_zbGroups, g_zbGroupItems, g_zbEngineItems;
struct BucketKey
{
    uint32_t sortKey, vertexFormat, flags, c0, f8, o84;
    uint8_t o8a;
    bool operator==(const BucketKey& o) const
    {
        return sortKey == o.sortKey && vertexFormat == o.vertexFormat && flags == o.flags && c0 == o.c0 && f8 == o.f8 && o84 == o.o84 &&
               o8a == o.o8a;
    }
};
struct BucketKeyHash
{
    size_t operator()(const BucketKey& k) const
    {
        uint64_t h = 1469598103934665603ull;
        for (const uint32_t v : {k.sortKey, k.vertexFormat, k.flags, k.c0, k.f8, k.o84, static_cast<uint32_t>(k.o8a)})
            h = (h ^ v) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

void ZListBucketed(uintptr_t r, int first, int last, uint32_t filter, uintptr_t items)
{
    const auto original = reinterpret_cast<ListFn>(kBatchListFn);
    const auto at = [items](int i) { return items + static_cast<uintptr_t>(i) * kRenderItemSize; };
    const auto key = [&](int i) { return *reinterpret_cast<const uint32_t*>(at(i)); };
    const auto re = [&](int i) { return *reinterpret_cast<const uintptr_t*>(at(i) + 8); };
    const auto matches = [&](int i) { return (*reinterpret_cast<const uint32_t*>(at(i) + 0x24) & filter) != 0; };
    static std::unordered_map<BucketKey, std::vector<int>, BucketKeyHash> buckets;
    static std::vector<BucketKey> order;
    for (const BucketKey& k : order)
        buckets[k].clear();
    order.clear();
    for (int i = first; i < last;)
    {
        if (!matches(i))
        {
            i++;
            continue;
        }
        int k = i + 1;
        while (k < last && key(k) == key(i) && re(k) == re(i))
            k++;
        if (k - i > 1)
        {
            original(r, i, k, filter); // a group sharing a render element: the engine batches it
            g_zbGroups++;
            g_zbGroupItems += static_cast<uint64_t>(k - i);
            g_zItems += static_cast<uint64_t>(k - i);
            i = k;
            continue;
        }
        const uintptr_t e = re(i), obj = *reinterpret_cast<const uintptr_t*>(at(i) + 0x18);
        const uintptr_t mesh = e && IsMeshRe(e) ? *reinterpret_cast<const uintptr_t*>(e + 0x48) : 0;
        if (!obj || !mesh || *reinterpret_cast<const int*>(mesh + kMeshPrimitive) != 0)
        {
            original(r, i, i + 1, filter);
            g_zbEngineItems++;
            g_zItems++;
            i++;
            continue;
        }
        BucketKey bk{};
        bk.sortKey = key(i);
        bk.vertexFormat = *reinterpret_cast<const uint32_t*>(mesh + kMeshVertexFormat);
        ObjectInputs(obj, bk.flags, bk.c0, bk.f8, bk.o84, bk.o8a);
        std::vector<int>& v = buckets[bk];
        if (v.empty())
            order.push_back(bk);
        v.push_back(i);
        i++;
    }
    ZRun run;
    const auto drawBase = [&](int idx) {
        g_lastPass = 0;
        g_objectLoops = g_instancedLoops = 0;
        original(r, idx, idx + 1, filter);
        g_zbBases++;
        g_zItems++;
        BeginRun(r, at(idx), g_objectLoops == 1 && g_instancedLoops == 0, run);
    };
    for (const BucketKey& k : order)
    {
        const std::vector<int>& v = buckets[k];
        g_zbBuckets++;
        drawBase(v[0]);
        for (size_t m = 1; m < v.size(); m++)
        {
            if (run.ok && OurItem(r, at(v[m]), run, original, v[m], filter))
                g_zItems++;
            else
                drawBase(v[m]);
        }
    }
}

void ZListOwn(uintptr_t r, int first, int last, uint32_t filter);

// The z-prepass's lists, named for frame debuggers as one range each (CryMT.cpp PassNameBegin).
void __fastcall ZListHook(uintptr_t r, int first, int last, uint32_t filter)
{
    const bool named = first != last;
    if (named)
        PassNameBegin(r, filter, g_zown ? "own z-prepass" : nullptr);
    ZListOwn(r, first, last, filter);
    if (named)
        PassNameEnd();
}

void ZListOwn(uintptr_t r, int first, int last, uint32_t filter)
{
    const auto original = reinterpret_cast<ListFn>(kBatchListFn);
    const bool oddFrame = (*reinterpret_cast<const uint32_t*>(r + kRdFrameId) & 1) != 0;
    if (!g_zown || filter != 8 || last - first < 2 || !PacketsReady() || (g_zownToggle && oddFrame == g_zownToggleEven))
    {
        original(r, first, last, filter);
        return;
    }
    ResolveTypes();
    const int list = *reinterpret_cast<const int*>(r + kRdBatchListId), afterWater = *reinterpret_cast<const int*>(r + 0xF0C);
    const uintptr_t items = *reinterpret_cast<const uintptr_t*>(kRenderItemLists + static_cast<uintptr_t>(afterWater * 15 + list) * 2 * 8);
    if (g_zBucket)
    {
        ZListBucketed(r, first, last, filter, items);
        return;
    }
    const auto at = [items](int i) { return items + static_cast<uintptr_t>(i) * kRenderItemSize; };
    const auto key = [&](int i) { return *reinterpret_cast<const uint32_t*>(at(i)); };
    const auto re = [&](int i) { return *reinterpret_cast<const uintptr_t*>(at(i) + 8); };
    const auto matches = [&](int i) { return (*reinterpret_cast<const uint32_t*>(at(i) + 0x24) & filter) != 0; };
    int i = first;
    while (i < last)
    {
        if (!matches(i))
        {
            i++;
            continue;
        }
        // The base group: consecutive items with this sort key and render element. Unless the engine would instance
        // it, only its first item goes to the original; the rest of the group are items of the run like any other.
        int k = i + 1;
        while (k < last && key(k) == key(i) && re(k) == re(i))
            k++;
        if (g_zGroups && k - i > 1 && (g_zGroupsAll || !PredictInstanced(key(i), k - i)))
        {
            DrawGroup(r, i, k, filter, items, original);
            i = k;
            continue;
        }
        if (k - i > 1)
        {
            if (!g_zownSplit || PredictInstanced(key(i), k - i))
                g_zGroupsInstanced++;
            else
            {
                k = i + 1;
                g_zGroupsTaken++;
            }
        }
        g_lastPass = 0;
        g_objectLoops = g_instancedLoops = 0;
        if (g_zownVerify)
        {
            g_zBaseWrites.clear();
            g_cbCapture = &g_zBaseWrites;
        }
        const bool predicted = k - i > 1 && PredictInstanced(key(i), k - i);
        g_loopObjects = 0;
        original(r, i, k, filter);
        g_cbCapture = nullptr;
        uint64_t* outcome = g_zBaseOutcome[predicted][g_objectLoops > 0][g_instancedLoops > 0];
        outcome[0]++;
        outcome[1] += static_cast<uint64_t>(k - i);
        outcome[2] += g_loopObjects;
        g_zItems += static_cast<uint64_t>(k - i);
        g_zBaseGroups++;
        g_zBaseItems += static_cast<uint64_t>(k - i);
        if (i == first || key(i) != key(i - 1))
        {
            // A new sort key: count the run's length (matching items with this key from here).
            int n = 0;
            for (int m = i; m < last && key(m) == key(i); m++)
                n += matches(m);
            g_zRuns++;
            g_zRunLen[n >= 16 ? 15 : n - 1]++;
        }
        ZRun run;
        // Usable when one plain per-object loop drew the group (its state is then that of a single item).
        BeginRun(r, at(k - 1), g_objectLoops == 1 && g_instancedLoops == 0, run);
        int j = k;
        while (run.ok && j < last && key(j) == run.sortKey)
        {
            if (!matches(j))
            {
                j++;
                continue;
            }
            if (j + 1 < last && key(j + 1) == run.sortKey && re(j + 1) == re(j))
            {
                // A group of its own: ours unless the engine would instance it (then the original, as a new run).
                int g = j + 1;
                while (g < last && key(g) == run.sortKey && re(g) == re(j))
                    g++;
                if (!g_zownSplit || PredictInstanced(run.sortKey, g - j))
                    break;
            }
            if (!OurItem(r, at(j), run, original, j, filter))
                break;
            g_zItems++;
            j++;
        }
        i = j;
    }
}

// Far vegetation sprites. The renderer builds its sprite list during the flush from the 3D engine's per-level sprite
// arrays (C3DEngine slot 40 -> FUN_300EEFE0 -> renderer slot 162 reads CObjManager+0x18+level*0x10, 0x18 bytes an
// entry) and draws that list in the z-prepass and the general pass (slot 39 -> FUN_300EEF80 -> renderer slot 161).
// Both helpers check the 3D engine's render recursion level (0x302008E0, set by RenderInner: 0 inside the main view,
// -1 outside) and the array's length, and pass the LOD factor 0x302008E4 (also set by RenderInner). The original
// flushes inside RenderInner on main, so they see the frame's values. CryMT flushes on the render thread: the list is
// built while main is held at EndEf3D (the preprocess callbacks), but the draws come later, when main has left
// RenderInner (level -1: skipped) or is filling the next frame. Measured on the gitest demo: z-prepass and general
// pass both drawn in 4% of flushes, neither in 50%, only the general pass (no z-prepass depth, not visible) in 42%,
// only the z-prepass in 3% (grey tree shapes: depth without colour, then fog). So the far forest flickered and was
// mostly missing. Fix (CRYMT_SPRITEFIX=0 turns it off): the draw uses the level, length and LOD factor recorded when
// the list was built. CRYMT_SPRITECHECK=1 logs every call with its thread, the array's length and a hash of its
// contents to logs\mt\crymt_sprites.txt.
// The level is read through CryMTRenderLevel (per-thread levels: each thread has its own, see CryMT.cpp).
constexpr uintptr_t k3DEngineVtable = 0x301D37E8, kObjManagerPtr = 0x3022B8C8, kSpriteLod = 0x302008E4,
                    k3DRendererPtr = 0x3022B8A0, k3DCVarsPtr = 0x3022B8E0;
bool g_spriteFix, g_spriteCheck;
int g_spriteLevel = -1;
uint32_t g_spriteCount;
float g_spriteLod, g_spriteViewDistance;
struct SpriteEvent
{
    uint64_t frame, hash;
    uint32_t thread, count;
    char kind;
    uint8_t level, zpass;
};
std::vector<SpriteEvent> g_spriteEvents;
SRWLOCK g_spriteLock = SRWLOCK_INIT;
bool g_spriteWritten;
uintptr_t g_spriteGen, g_spriteDraw;

void WriteSpriteEvents()
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("crymt_sprites.txt").c_str(), "w") != 0 || !f)
        return;
    fprintf(f, "world frame, thread, G = list generated / D = drawn, level, z-prepass, entries, hash of the entries\n");
    for (const SpriteEvent& e : g_spriteEvents)
        fprintf(f, "%llu %u %c %u %u %u %016llx\n", static_cast<unsigned long long>(e.frame), e.thread, e.kind, e.level, e.zpass, e.count,
                static_cast<unsigned long long>(e.hash));
    fclose(f);
}

void NoteSprites(char kind)
{
    SpriteEvent e{::CryMTWorldFrame(), 1469598103934665603ull, GetCurrentThreadId(), 0, kind, 0, 0};
    const uintptr_t objects = *reinterpret_cast<const uintptr_t*>(kObjManagerPtr);
    const int level = ::CryMTRenderLevel();
    e.level = static_cast<uint8_t>(level);
    if (objects && level >= 0 && level < 2)
    {
        const uintptr_t list = objects + 0x18 + level * 0x10;
        const auto* data = *reinterpret_cast<const uint8_t* const*>(list);
        e.count = *reinterpret_cast<const uint32_t*>(list + 8);
        for (size_t i = 0; data && i < e.count * size_t{0x18}; i++)
            e.hash = (e.hash ^ data[i]) * 1099511628211ull;
    }
    const uintptr_t renderer = *reinterpret_cast<const uintptr_t*>(k3DRendererPtr);
    e.zpass = renderer && (*reinterpret_cast<const uint8_t*>(renderer + 0xF00) & 8);
    AcquireSRWLockExclusive(&g_spriteLock);
    g_spriteEvents.push_back(e);
    const bool write = !g_spriteWritten && (g_spriteEvents.size() >= 12000 || e.frame >= 2190);
    g_spriteWritten |= write;
    ReleaseSRWLockExclusive(&g_spriteLock);
    if (write)
        WriteSpriteEvents();
}

// C3DEngine slot 45 (0x300F6DD0): the view distance for the camera's height above the water, from ISystem's view
// camera, which main moves on to the next frame before the draws.
using ViewDistanceFn = float(__fastcall*)(void*);

// Each entry of the renderer's sprite list (0x382E0ED8, count 0x382E0EE0, 0x38 bytes) keeps at +8 the 3D engine's
// pointer to the texture set of the terrain sector under the tree (CTerrainNode+0x11C: two ushort texture ids, then
// offsets and scale, 0x18 bytes); the draw binds that sector texture for the outdoor AO (SpritesOutdoorAOVertInfo).
// Main reassigns sector textures for the next frame while the draws run (the new contents queued behind the flush),
// so trees took another sector's texture for a frame (lighter patches every ~23 frames on the gitest demo end). The
// sets are copied while main is held and the entries point at the copies; one copy per set, as the draw batches
// neighbouring entries with the same pointer.
constexpr uintptr_t kSpriteList = 0x382E0ED8, kSpriteListCount = 0x382E0EE0;
constexpr size_t kSpriteListEntry = 0x38, kSectorTextureSet = 0x18;
std::vector<uint8_t> g_spriteTexSets;
std::unordered_map<const uint8_t*, size_t> g_spriteTexSetIndex;

void CopySpriteTextureSets()
{
    const int count = *reinterpret_cast<const int*>(kSpriteListCount);
    uint8_t* list = *reinterpret_cast<uint8_t* const*>(kSpriteList);
    if (!list || count <= 0)
        return;
    g_spriteTexSetIndex.clear();
    for (int i = 0; i < count; i++)
        if (const uint8_t* set = *reinterpret_cast<const uint8_t* const*>(list + i * kSpriteListEntry + 8))
            g_spriteTexSetIndex.emplace(set, g_spriteTexSetIndex.size());
    g_spriteTexSets.resize(g_spriteTexSetIndex.size() * kSectorTextureSet); // before any entry points into it
    for (const auto& [set, index] : g_spriteTexSetIndex)
        memcpy(&g_spriteTexSets[index * kSectorTextureSet], set, kSectorTextureSet);
    for (int i = 0; i < count; i++)
    {
        auto** set = reinterpret_cast<const uint8_t**>(list + i * kSpriteListEntry + 8);
        if (*set)
            *set = &g_spriteTexSets[g_spriteTexSetIndex[*set] * kSectorTextureSet];
    }
}

void __fastcall SpriteGenHook(void* engine)
{
    g_spriteLevel = ::CryMTRenderLevel();
    g_spriteLod = *reinterpret_cast<const float*>(kSpriteLod);
    g_spriteViewDistance = (*reinterpret_cast<ViewDistanceFn* const*>(engine))[45](engine);
    const uintptr_t objects = *reinterpret_cast<const uintptr_t*>(kObjManagerPtr);
    g_spriteCount = objects && g_spriteLevel >= 0 && g_spriteLevel < 2 ? *reinterpret_cast<const uint32_t*>(objects + 0x20 + g_spriteLevel * 0x10) : 0;
    if (g_spriteCheck)
        NoteSprites('G');
    reinterpret_cast<void(__fastcall*)(void*)>(g_spriteGen)(engine);
    if (g_spriteFix)
        CopySpriteTextureSets();
}

// FUN_300EEF80 with the values recorded when the list was built (slot 39 calls slot 45 first, then it).
void __fastcall SpriteDrawHook(void* engine)
{
    if (g_spriteCheck)
        NoteSprites('D');
    if (!g_spriteFix)
    {
        reinterpret_cast<void(__fastcall*)(void*)>(g_spriteDraw)(engine);
        return;
    }
    const uintptr_t cvars = *reinterpret_cast<const uintptr_t*>(k3DCVarsPtr);
    if (!cvars || !*reinterpret_cast<const int*>(cvars + 0xA0) || g_spriteLevel < 0 || g_spriteLevel > 1 || !g_spriteCount)
        return;
    const uintptr_t objects = *reinterpret_cast<const uintptr_t*>(kObjManagerPtr);
    void* renderer = *reinterpret_cast<void* const*>(k3DRendererPtr);
    using DrawFn = void(__fastcall*)(void*, uintptr_t, float, uintptr_t, float);
    (*reinterpret_cast<DrawFn* const*>(renderer))[161](renderer, objects + 0x18 + g_spriteLevel * 0x10, g_spriteViewDistance, objects, g_spriteLod);
}

void InstallSpriteCheck()
{
    wchar_t text[8];
    g_spriteFix = !(GetEnvironmentVariableW(L"CRYMT_SPRITEFIX", text, 8) && wcscmp(text, L"0") == 0);
    g_spriteCheck = GetEnvironmentVariableW(L"CRYMT_SPRITECHECK", nullptr, 0) != 0;
    if (!g_spriteFix && !g_spriteCheck)
        return;
    auto* vtable = reinterpret_cast<uintptr_t*>(k3DEngineVtable);
    if (vtable[39] != 0x30108D30 || vtable[40] != 0x30108D50)
        return;
    if (g_spriteCheck)
        g_spriteEvents.reserve(12000);
    DWORD old;
    VirtualProtect(vtable + 39, 16, PAGE_READWRITE, &old);
    g_spriteDraw = vtable[39];
    g_spriteGen = vtable[40];
    vtable[39] = reinterpret_cast<uintptr_t>(&SpriteDrawHook);
    vtable[40] = reinterpret_cast<uintptr_t>(&SpriteGenHook);
    VirtualProtect(vtable + 39, 16, old, &old);
}
} // namespace

void OnEngineLoaded()
{
    InstallProcHooks();
    InstallSpriteCheck();
}

void Install()
{
    wchar_t text[16];
    // A switch is on when set to anything but 0 (its value stays in text for the toggles).
    const auto on = [&text](const wchar_t* name) { return GetEnvironmentVariableW(name, text, 16) != 0 && wcscmp(text, L"0") != 0; };
    g_piVerify = GetEnvironmentVariableW(L"CRYMT_PIVERIFY", nullptr, 0) != 0;
    g_packets = !g_piVerify && on(L"CRYMT_ZPACKETS");
    g_piFast = !g_piVerify && GetEnvironmentVariableW(L"CRYMT_PIFAST", nullptr, 0) != 0;
    g_census = GetEnvironmentVariableW(L"CRYMT_ZCENSUS", nullptr, 0) != 0;
    g_traceTarget = GetEnvironmentVariableW(L"CRYMT_ZTRACE", text, 16) ? static_cast<uint32_t>(_wtoi(text)) : 0;
    g_cycles = GetEnvironmentVariableW(L"CRYMT_ZCYCLES", nullptr, 0) != 0;
    g_loopCensus = GetEnvironmentVariableW(L"CRYMT_LOOPCENSUS", nullptr, 0) != 0;
    g_mergeCensus = GetEnvironmentVariableW(L"CRYMT_MERGECENSUS", nullptr, 0) != 0;
    g_objTexCensus = GetEnvironmentVariableW(L"CRYMT_OBJTEXCENSUS", nullptr, 0) != 0;
    g_texRelax = on(L"CRYMT_TEXRELAX");
    g_texRelaxEven = g_texRelax && _wcsicmp(text, L"toggle2") == 0;
    g_texRelaxToggle = g_texRelax && (_wcsicmp(text, L"toggle") == 0 || g_texRelaxEven);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_qpcStart = static_cast<uint64_t>(q.QuadPart);
    g_tscStart = __rdtsc();
    const auto detour = [](uintptr_t fn, const uint8_t* head, size_t len, const void* hook) { return DetourHead(fn, head, len, hook); };
    if (g_mergeCensus || g_texRelax)
        g_merge = reinterpret_cast<MergeFn>(detour(kInstanceMergeFn, kMergeHead, sizeof(kMergeHead), reinterpret_cast<const void*>(&MergeHook)));
    g_mergeCensus = g_mergeCensus && g_merge;
    if (g_piVerify)
        if (auto* t = reinterpret_cast<CbWriteFn>(detour(kCbWriteFn, kCbWriteHead, sizeof(kCbWriteHead), reinterpret_cast<const void*>(&CbWriteHook))))
            g_cbWrite = t;
    if (g_piFast || g_piVerify || g_cycles)
        g_setGroup = reinterpret_cast<SetGroupFn>(
            detour(kShaderSetGroupFn, kShaderSetGroupHead, sizeof(kShaderSetGroupHead), reinterpret_cast<const void*>(&SetGroupHook)));
    if (g_packets || g_cycles)
        g_meshDraw = reinterpret_cast<MeshDrawFn>(detour(kMeshDrawFn, kMeshDrawHead, sizeof(kMeshDrawHead), reinterpret_cast<const void*>(&MeshDrawHook)));
    g_zownVerify = GetEnvironmentVariableW(L"CRYMT_ZOWNVERIFY", nullptr, 0) != 0;
    g_zown = g_zownVerify || on(L"CRYMT_ZOWN");
    g_zownToggleEven = g_zown && !g_zownVerify && _wcsicmp(text, L"toggle2") == 0;
    g_zownToggle = g_zown && !g_zownVerify && (_wcsicmp(text, L"toggle") == 0 || g_zownToggleEven);
    g_zownSplit = !(GetEnvironmentVariableW(L"CRYMT_ZOWNSPLIT", text, 16) && wcscmp(text, L"0") == 0);
    g_zBucket = g_zown && GetEnvironmentVariableW(L"CRYMT_ZBUCKET", nullptr, 0) != 0;
    g_zGroupVerify = g_zown && GetEnvironmentVariableW(L"CRYMT_ZGROUPVERIFY", nullptr, 0) != 0;
    const bool groupsVar = on(L"CRYMT_ZGROUPS");
    g_zGroups = g_zown && (g_zGroupVerify || groupsVar);
    g_zGroupsAll = g_zGroups && groupsVar && _wcsicmp(text, L"all") == 0;
    g_zGroupRelax = g_zGroups && on(L"CRYMT_ZGROUPRELAX");
    g_zGroupDefer = !on(L"CRYMT_ZGROUPVALUES");
    g_loopGroupVerify = g_packets && GetEnvironmentVariableW(L"CRYMT_LOOPGROUPVERIFY", nullptr, 0) != 0;
    g_loopGroups = g_packets && (g_loopGroupVerify || on(L"CRYMT_LOOPGROUPS"));
    g_loopGroupsToggle = g_loopGroups && !g_loopGroupVerify && _wcsicmp(text, L"toggle") == 0;
    const bool drawVar = GetEnvironmentVariableW(L"CRYMT_ZOWNDRAW", text, 16) != 0;
    g_zownEngineDraw = drawVar && _wcsicmp(text, L"engine") == 0;
    g_zownFlushDraw = drawVar && _wcsicmp(text, L"flush") == 0;
    if (g_packets || g_cycles || g_traceTarget || g_zown || g_loopCensus || g_objTexCensus || g_texRelax)
        g_drawObjects = reinterpret_cast<DrawObjectsFn>(
            detour(kDrawObjectsFn, kDrawObjectsHead, sizeof(kDrawObjectsHead), reinterpret_cast<const void*>(&DrawObjectsHook)));
    if (g_zown)
    {
        g_drawObjectsAlt = reinterpret_cast<DrawObjectsAltFn>(
            detour(kDrawObjectsAltFn, kDrawObjectsAltHead, sizeof(kDrawObjectsAltHead), reinterpret_cast<const void*>(&DrawObjectsAltHook)));
        int patched = 0;
        if (g_drawObjects && g_drawObjectsAlt)
            for (const uintptr_t site : kZPrepassListCalls)
                patched += PatchCall(site, kBatchListFn, reinterpret_cast<const void*>(&ZListHook));
        g_zown = patched == 6;
    }
    if (g_texRelax && !g_drawObjectsAlt)
        g_drawObjectsAlt = reinterpret_cast<DrawObjectsAltFn>(
            detour(kDrawObjectsAltFn, kDrawObjectsAltHead, sizeof(kDrawObjectsAltHead), reinterpret_cast<const void*>(&DrawObjectsAltHook)));
    g_texRelax = g_texRelax && g_merge && g_drawObjects && g_drawObjectsAlt;
    g_packets = g_packets && g_setGroup && g_meshDraw && g_drawObjects;
    if (g_census || g_traceTarget || g_cycles || g_loopCensus || g_mergeCensus || g_objTexCensus)
        g_zPrepass = reinterpret_cast<ZPrepassFn>(detour(kZPrepassFn, kZPrepassHead, sizeof(kZPrepassHead), reinterpret_cast<const void*>(&ZPrepassHook)));
    if (g_census || g_traceTarget)
        g_setParams = reinterpret_cast<SetParamsFn>(detour(kSetParamsFn, kSetParamsHead, sizeof(kSetParamsHead), reinterpret_cast<const void*>(&SetParamsHook)));
    if (g_traceTarget || g_cycles)
        g_flush = reinterpret_cast<FlushFn>(detour(kFlushShaderFn, kFlushShaderHead, sizeof(kFlushShaderHead), reinterpret_cast<const void*>(&FlushHook)));
    g_setVerify = GetEnvironmentVariableW(L"CRYMT_SETVERIFY", nullptr, 0) != 0;
    g_setReuse = !g_setVerify && GetEnvironmentVariableW(L"CRYMT_SETREUSE", nullptr, 0) != 0;
    if ((g_setVerify || g_zownVerify || g_zGroupVerify || g_loopGroupVerify) && g_cbWrite == reinterpret_cast<CbWriteFn>(kCbWriteFn))
        if (auto* t = reinterpret_cast<CbWriteFn>(detour(kCbWriteFn, kCbWriteHead, sizeof(kCbWriteHead), reinterpret_cast<const void*>(&CbWriteHook))))
            g_cbWrite = t;
    if (g_setReuse || g_setVerify)
        g_shaderSet = reinterpret_cast<ShaderSetFn>(detour(kTimedSites[0].fn, kTimedSites[0].head, kTimedSites[0].len, reinterpret_cast<const void*>(&ShaderSetHook)));
    if (g_cycles)
        InstallTimed(std::make_integer_sequence<int, static_cast<int>(sizeof(kTimedSites) / sizeof(kTimedSites[0]))>{}, g_shaderSet != nullptr);
    if (g_traceTarget)
    {
        g_trace.reserve(1 << 20);
        g_objectChange = reinterpret_cast<ObjectChangeFn>(
            detour(kObjectChangeFn, kObjectChangeHead, sizeof(kObjectChangeHead), reinterpret_cast<const void*>(&ObjectChangeHook)));
        if (!g_drawObjectsAlt)
            g_drawObjectsAlt = reinterpret_cast<DrawObjectsAltFn>(
                detour(kDrawObjectsAltFn, kDrawObjectsAltHead, sizeof(kDrawObjectsAltHead), reinterpret_cast<const void*>(&DrawObjectsAltHook)));
    }
}

void WriteStats()
{
    if (!g_zPrepass && !g_setParams && !g_setGroup && !g_meshDraw && !g_shaderSet && !g_zown && !g_procCensus && !g_texRelax)
        return;
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("zpass_census.txt").c_str(), "w") != 0 || !f)
        return;
    if (g_texRelax)
        fprintf(f, "relaxed texture merge (%s): objects joined %llu, refused: objdata into mixed %llu, +0xCE sign %llu; loop path: mixed %llu (PS reads "
                   "no object texture %llu), rebinds %llu (failed %llu); instanced path: mixed %llu, runs %llu; runs in all %llu; instancing threshold %u\n",
                g_texRelaxToggle ? "toggle" : "on", static_cast<unsigned long long>(g_trJoined), static_cast<unsigned long long>(g_trNoObjdata),
                static_cast<unsigned long long>(g_trRefused), static_cast<unsigned long long>(g_trMixed), static_cast<unsigned long long>(g_trSameTex),
                static_cast<unsigned long long>(g_trRebinds), static_cast<unsigned long long>(g_trResFail), static_cast<unsigned long long>(g_trAltMixed),
                static_cast<unsigned long long>(g_trAltRuns), static_cast<unsigned long long>(g_trRuns), *reinterpret_cast<const uint32_t*>(0x382E9B64));
    WriteLoopCensus(f);
    WriteMergeCensus(f);
    WriteProcCensus(f);
    if (g_zown)
    {
        fprintf(f, "own z-prepass (%s): items %llu, ours %llu (%.1f%%), skipped by the object change %llu; packets sent %llu\n",
                g_zownVerify ? "verify" : "on", static_cast<unsigned long long>(g_zItems), static_cast<unsigned long long>(g_zOurs),
                g_zItems ? 100.0 * static_cast<double>(g_zOurs) / static_cast<double>(g_zItems) : 0.0, static_cast<unsigned long long>(g_zSkipped),
                static_cast<unsigned long long>(g_packetsSent));
        for (int i = 0; i < kRejCount; i++)
            fprintf(f, "  not ours: %-36s %llu\n", kRejectNames[i], static_cast<unsigned long long>(g_zReject[i]));
        fprintf(f, "  own streams %llu (verified %llu, mismatches %llu), constant commits skipped %llu; needed for dirty matrices %llu, pending %llu, "
                   "unbound %llu, material/object data %llu, lights %llu\n",
                static_cast<unsigned long long>(g_zOwnStreams), static_cast<unsigned long long>(g_zStreamsCompared),
                static_cast<unsigned long long>(g_zStreamsMismatch), static_cast<unsigned long long>(g_zCommitSkipped),
                static_cast<unsigned long long>(g_commitWhy[0]), static_cast<unsigned long long>(g_commitWhy[1]),
                static_cast<unsigned long long>(g_commitWhy[2]), static_cast<unsigned long long>(g_commitWhy[3]),
                static_cast<unsigned long long>(g_commitWhy[4]));
        if (g_loopGroups)
        {
            fprintf(f, "  loop groups: batches %llu, merged objects ours %llu; not ours:", static_cast<unsigned long long>(g_lgBatches),
                    static_cast<unsigned long long>(g_lgObjects));
            for (int i = 0; i < 7; i++)
                fprintf(f, " %s %llu,", kLoopRejectNames[i], static_cast<unsigned long long>(g_lgReject[i]));
            fprintf(f, "\n");
            if (g_loopGroupVerify)
            {
                fprintf(f, "  loop group verify: values compared %llu, mismatches %llu, not written by the engine %llu, other writes %llu\n",
                        static_cast<unsigned long long>(g_lgCompared), static_cast<unsigned long long>(g_lgMismatch),
                        static_cast<unsigned long long>(g_lgMissing), static_cast<unsigned long long>(g_lgOther));
                for (const auto& [k, v] : g_lgByType)
                    fprintf(f, "    %10llu  %s\n", static_cast<unsigned long long>(v), k.c_str());
                fprintf(f, "%s", g_lgLog.c_str());
            }
        }
        if (g_zGroups)
        {
            fprintf(f, "  groups: %llu with %llu objects ours (drawn by the stream %llu, values not computable %llu), cut short %llu; not ours:",
                    static_cast<unsigned long long>(g_zgGroups), static_cast<unsigned long long>(g_zgObjects),
                    static_cast<unsigned long long>(g_zgDrawn.load()), static_cast<unsigned long long>(g_zgResolveFailed.load()),
                    static_cast<unsigned long long>(g_zgCut));
            for (int i = 0; i < 7; i++)
                fprintf(f, " %s %llu,", kGroupRejectNames[i], static_cast<unsigned long long>(g_zgReject[i]));
            fprintf(f, "\n");
            if (g_zGroupRelax)
                fprintf(f, "  relaxed texture merge: batches allowed %llu, objects joined %llu, blocked by flag 0x1000 %llu\n",
                        static_cast<unsigned long long>(g_zgRelaxBatches), static_cast<unsigned long long>(g_zgRelaxed),
                        static_cast<unsigned long long>(g_zgRelaxBlocked));
            fprintf(f, "  next object not merging, first differing field:");
            for (int i = 0; i < 8; i++)
                fprintf(f, " %s %llu,", kMergeFieldNames[i], static_cast<unsigned long long>(g_zgMergeFail[i]));
            fprintf(f, "\n");
            if (g_zGroupVerify)
                fprintf(f, "  group verify: engine values compared %llu, mismatches %llu, not written by the engine %llu; stream vs render thread %llu, mismatches %llu; "
                           "other shader instances %llu; per-batch writes compared %llu, differing %llu, not written by the base %llu\n",
                        static_cast<unsigned long long>(g_zgVerifyCompared), static_cast<unsigned long long>(g_zgVerifyMismatch),
                        static_cast<unsigned long long>(g_zgVerifyCount), static_cast<unsigned long long>(g_zgRaceCompared.load()),
                        static_cast<unsigned long long>(g_zgRaceMismatch.load()), static_cast<unsigned long long>(g_zgInstanceDiff),
                        static_cast<unsigned long long>(g_zgBatchCompared), static_cast<unsigned long long>(g_zgBatchDiff),
                        static_cast<unsigned long long>(g_zgBatchMissing));
        }
        if (g_zBucket)
            fprintf(f, "  bucketed: buckets %llu, bases %llu (incl. re-bases after a fallback), groups to the engine %llu with %llu items, other items to the engine %llu\n",
                    static_cast<unsigned long long>(g_zbBuckets), static_cast<unsigned long long>(g_zbBases), static_cast<unsigned long long>(g_zbGroups),
                    static_cast<unsigned long long>(g_zbGroupItems), static_cast<unsigned long long>(g_zbEngineItems));
        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++)
                for (int c = 0; c < 2; c++)
                    if (g_zBaseOutcome[a][b][c][0])
                        fprintf(f, "  original calls (instancing predicted %d, plain loops %d, instanced loops %d): %llu calls, %llu items, %llu plain-loop objects\n",
                                a, b, c, static_cast<unsigned long long>(g_zBaseOutcome[a][b][c][0]), static_cast<unsigned long long>(g_zBaseOutcome[a][b][c][1]),
                                static_cast<unsigned long long>(g_zBaseOutcome[a][b][c][2]));
        fprintf(f, "  multi-item groups taken (base item only to the original) %llu, left to instancing %llu\n",
                static_cast<unsigned long long>(g_zGroupsTaken), static_cast<unsigned long long>(g_zGroupsInstanced));
        fprintf(f, "  original groups %llu with %llu items; sort-key runs %llu, by matching length 1..16+:", static_cast<unsigned long long>(g_zBaseGroups),
                static_cast<unsigned long long>(g_zBaseItems), static_cast<unsigned long long>(g_zRuns));
        for (const uint64_t n : g_zRunLen)
            fprintf(f, " %llu", static_cast<unsigned long long>(n));
        fprintf(f, "\n");
        fprintf(f, "  runs without: shader %llu, object %llu, CREMesh %llu, VS %llu\n", static_cast<unsigned long long>(g_zRunWhy[0]),
                static_cast<unsigned long long>(g_zRunWhy[1]), static_cast<unsigned long long>(g_zRunWhy[2]), static_cast<unsigned long long>(g_zRunWhy[3]));
        for (const auto& [vt, s15] : g_zSeenVt)
            fprintf(f, "    element vtable %llx slot 15 %llx\n", static_cast<unsigned long long>(vt), static_cast<unsigned long long>(s15));
        if (g_zownVerify)
        {
            fprintf(f, "  verified items %llu; state calls the original made on our items:\n", static_cast<unsigned long long>(g_zVerified));
            for (const auto& [name, n] : g_zVerifyOps)
                fprintf(f, "    %10llu  %s\n", static_cast<unsigned long long>(n), name.c_str());
            fprintf(f, "  constants: per-instance compared %llu, mismatches %llu; others compared %llu, mismatches %llu, not written by the base %llu\n",
                    static_cast<unsigned long long>(g_zPiCompared), static_cast<unsigned long long>(g_zPiMismatch),
                    static_cast<unsigned long long>(g_zPbCompared), static_cast<unsigned long long>(g_zPbMismatch),
                    static_cast<unsigned long long>(g_zPbMissing));
            fprintf(f, "%s", g_zMismatchLog.c_str());
            fprintf(f, "  our items the original did not draw: %llu\n%s", static_cast<unsigned long long>(g_zOrigNoDraw), g_zNoDrawLog.c_str());
            fprintf(f, "  our items whose per-instance values the original did not write: %llu\n%s", static_cast<unsigned long long>(g_zPiNotWritten),
                    g_zNotWrittenLog.c_str());
        }
    }
    if (g_shaderSet)
        fprintf(f, "vf7 reuse (%s): calls %llu, reused %llu (%.1f%%); verified candidates %llu: other instance %llu, other per-batch constants %llu, "
                   "device state calls %llu\n",
                g_setVerify ? "verify" : "fast", static_cast<unsigned long long>(g_vf7Calls), static_cast<unsigned long long>(g_setReused),
                g_vf7Calls ? 100.0 * static_cast<double>(g_setReused + g_setCandidates) / static_cast<double>(g_vf7Calls) : 0.0,
                static_cast<unsigned long long>(g_setCandidates), static_cast<unsigned long long>(g_setInstDiff), static_cast<unsigned long long>(g_setPbDiff),
                static_cast<unsigned long long>(g_setD3dDiff));
    if (g_cycles)
    {
        LARGE_INTEGER q, freq;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&freq);
        const double seconds = static_cast<double>(static_cast<uint64_t>(q.QuadPart) - g_qpcStart) / static_cast<double>(freq.QuadPart);
        const double tscPerMs = seconds > 0 ? static_cast<double>(__rdtsc() - g_tscStart) / (seconds * 1000.0) : 1.0;
        const double passes = g_cyCalls[kCyZPrepass] ? static_cast<double>(g_cyCalls[kCyZPrepass]) : 1.0;
        fprintf(f, "cycles (render thread, inclusive; %.0f cycles per ms; per z-prepass = per frame):\n", tscPerMs);
        for (int i = 0; i < kCySlots; i++)
            fprintf(f, "  %-32s calls %10llu (%7.1f per frame), %7.0f cycles per call, %6.3f ms per frame\n", kCycleNames[i],
                    static_cast<unsigned long long>(g_cyCalls[i]), g_cyCalls[i] / passes,
                    g_cyCalls[i] ? static_cast<double>(g_cyTicks[i]) / static_cast<double>(g_cyCalls[i]) : 0.0, g_cyTicks[i] / passes / tscPerMs);
        const double fl = g_flushes ? static_cast<double>(g_flushes) : 1.0;
        fprintf(f, "flush repeats: %llu flushes; same shader as the previous %.1f%%, same setup key %.1f%%, same shader and instances %.1f%%, "
                   "same key but other instances %llu\n",
                static_cast<unsigned long long>(g_flushes), 100.0 * g_sameShader / fl, 100.0 * g_sameKey / fl, 100.0 * g_sameInst / fl,
                static_cast<unsigned long long>(g_sameKeyOtherInst));
    }
    if (g_setGroup)
    {
        fprintf(f, "per-instance parameters (%s): group calls %llu, ours %llu, verified %llu, mismatches %llu, not written by the original %llu; "
                   "PI_ObjMatrix = type %d\n",
                g_piVerify ? "verify" : "fast", static_cast<unsigned long long>(g_piCalls), static_cast<unsigned long long>(g_piFast_),
                static_cast<unsigned long long>(g_piVerified), static_cast<unsigned long long>(g_piMismatches), static_cast<unsigned long long>(g_piUnseen),
                g_tObjMatrix);
        fprintf(f, "  cycles per handled group (%s): %.0f over %llu groups\n", g_piVerify ? "the original's" : "ours",
                g_piTimed ? static_cast<double>(g_piTicks) / static_cast<double>(g_piTimed) : 0.0, static_cast<unsigned long long>(g_piTimed));
        if (g_packets)
            fprintf(f, "draw packets: back end %d, sent %llu, mesh draws left to the engine with held groups %llu, held groups written the engine's way %llu, "
                       "dropped (no draw) %llu\n",
                    g_packetsReady, static_cast<unsigned long long>(g_packetsSent), static_cast<unsigned long long>(g_packetFallbacks),
                    static_cast<unsigned long long>(g_heldFlushed), static_cast<unsigned long long>(g_heldDropped));
        fprintf(f, "  groups left to the original, by their first unknown type:\n");
        for (const auto& [t, n] : g_piUnsupported)
            fprintf(f, "    %10llu  %s\n", static_cast<unsigned long long>(n), t < 0 ? "(count)" : TypeNames(static_cast<uint32_t>(t)).c_str());
        if (!g_mismatchLog.empty())
            fprintf(f, "  first mismatches:\n%s", g_mismatchLog.c_str());
    }
    AcquireSRWLockShared(&g_lock);
    const double passes = g_passes ? static_cast<double>(g_passes) : 1.0;
    fprintf(f, "hooks: z-prepass %d, parameter setter %d\n", g_zPrepass != nullptr, g_setParams != nullptr);
    fprintf(f, "z-prepasses %llu; per pass: parameter set calls %.1f, parameters %.1f\n", static_cast<unsigned long long>(g_passes),
            g_setCalls / passes, g_setParamsTotal / passes);
    static const char* const kStages[3] = {"VS", "PS", "GS"};
    for (int s = 0; s < 3; s++)
        for (int c = 0; c < 64; c++)
            if (g_slotCalls[s][c])
                fprintf(f, "  %s cb of %d vectors: %.1f calls per pass\n", kStages[s], c, g_slotCalls[s][c] / passes);
    // By parameter type over all shaders, then every (shader, stage, slot, register) row.
    std::unordered_map<std::string, uint64_t> byType;
    for (const auto& [k, v] : g_params)
        byType[std::string(k.stage >= 0 && k.stage < 3 ? kStages[k.stage] : "?") + " cb " + std::to_string(k.slot) + " " + v.types] += v.count;
    fprintf(f, "parameter types (stage, cb vectors, type: per pass):\n");
    for (const auto& [t, n] : byType)
        fprintf(f, "  %8.1f  %s\n", n / passes, t.c_str());
    fprintf(f, "rows (per pass, shader, stage, cb vectors, register float offset, floats, flags, types):\n");
    for (const auto& [k, v] : g_params)
        fprintf(f, "  %8.2f  %-24s %s cb %d at %-3d x%d flags 0x%x  %s\n", v.count / passes, v.shader.c_str(),
                k.stage >= 0 && k.stage < 3 ? kStages[k.stage] : "?", k.slot, k.reg, k.vectors, k.flags, v.types.c_str());
    ReleaseSRWLockShared(&g_lock);
    fclose(f);
}
} // namespace zpass

void ZPassExpandGroup(const uint8_t* payload)
{
    zpass::ExpandGroup(payload);
}
