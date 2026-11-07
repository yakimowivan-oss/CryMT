// The 64-bit memory fix inside CryMT, for Bin64\Crysis.exe without c1-launcher v8 (GOG's c1-launcher v7).
//
// The engine's node allocator (CryMemoryAllocator, inlined all over the engine: STLport's default allocator, Lua's,
// FMOD's) keeps the pointers of its free lists through 32-bit moves. A block above 4 GB corrupts them, and the game
// crashes once it uses that much memory (stock 64-bit Crysis: 3 of 3 CPU benchmark runs). The allocator takes its
// memory from CryMalloc in blocks of exactly 0x80000 bytes. Those blocks come from a pool below 4 GB here.
//
// How the engine reaches it: every engine module looks for CryMalloc, CryRealloc, CryFree, CryGetMemSize,
// CrySystemCrtMalloc and CrySystemCrtFree among the exports of the executable first (GetModuleHandle(0)), then of
// CrySystem.dll (CryMemoryManager_impl.h in the Mod SDK). c1-launcher v8 exports its own; with v7 the executable has
// none. ExportFromExe adds the functions below to the executable's export table in memory, so every module (CrySystem
// too) finds them exactly where it finds v8's; they pass everything but the pool's blocks on to CrySystem's exports.
//
// CrySystem's own allocator also takes 0x80000 blocks through these functions and hands out small pieces of them, whose
// frees come back through CryFree; so only the exact start of a block the pool handed out goes back to the pool, and
// any other address goes to CrySystem (a first try took every address inside the pool for its block, freed whole blocks
// under live strings and crashed in CrySystem at start; passing CrySystem's own requests by the pool left the crash of
// the stock game in place).
//
// The pool: CRYMT_SAFEPOOL_MB (default 2048; 0 turns the fix off) reserved in 64 MB pieces from 0x40000000 upward,
// above the engine modules' bases (0x30000000-0x39741000), around the editor's ToolkitPro (0x67800000). Blocks are
// committed when handed out and decommitted when freed.
#include "MemoryFix.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace memfix
{
namespace
{
constexpr size_t kBlock = 0x80000;
constexpr size_t kPiece = 64u << 20;

struct Range
{
    uintptr_t base, end;
};
std::vector<Range> g_ranges;     // reserved pieces, address order
std::vector<uint32_t> g_free;    // free block numbers (stack)
std::vector<uintptr_t> g_blocks; // block number -> address
std::vector<uint8_t> g_out;      // block number -> handed out (under g_lock)
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_active;
std::atomic<int64_t> g_inUse, g_peak, g_allocs, g_frees, g_missed, g_commitFailed;

using MallocFn = void*(__cdecl*)(size_t, size_t&);
using ReallocFn = void*(__cdecl*)(void*, size_t, size_t&);
using FreeFn = size_t(__cdecl*)(void*);
using SizeFn = size_t(__cdecl*)(void*, size_t);
using CrtMallocFn = void*(__cdecl*)(size_t);
using CrtFreeFn = void(__cdecl*)(void*);
using CrtSizeFn = size_t(__cdecl*)(void*);
struct System
{
    MallocFn malloc;
    ReallocFn realloc;
    FreeFn free;
    SizeFn size;
    CrtMallocFn crtMalloc;
    CrtFreeFn crtFree;
    CrtSizeFn crtSize;
};

// CrySystem's own functions, found once it is loaded (the engine modules that call these load after it).
System g_sys;
std::atomic<bool> g_sysReady;
const System& Sys()
{
    if (!g_sysReady.load(std::memory_order_acquire))
    {
        const HMODULE sys = GetModuleHandleW(L"CrySystem.dll");
        if (sys)
        {
            System r{};
            r.malloc = reinterpret_cast<MallocFn>(GetProcAddress(sys, "CryMalloc"));
            r.realloc = reinterpret_cast<ReallocFn>(GetProcAddress(sys, "CryRealloc"));
            r.free = reinterpret_cast<FreeFn>(GetProcAddress(sys, "CryFree"));
            r.size = reinterpret_cast<SizeFn>(GetProcAddress(sys, "CryGetMemSize"));
            r.crtMalloc = reinterpret_cast<CrtMallocFn>(GetProcAddress(sys, "CrySystemCrtMalloc"));
            r.crtFree = reinterpret_cast<CrtFreeFn>(GetProcAddress(sys, "CrySystemCrtFree"));
            r.crtSize = reinterpret_cast<CrtSizeFn>(GetProcAddress(sys, "CrySystemCrtSize"));
            if (r.malloc && r.free)
            {
                g_sys = r;
                g_sysReady.store(true, std::memory_order_release);
            }
        }
    }
    return g_sys;
}

bool InRanges(uintptr_t a)
{
    for (const Range& r : g_ranges)
        if (a >= r.base && a < r.end)
            return true;
    return false;
}

// The number of a block the pool handed out that starts exactly at p, else -1.
ptrdiff_t OurBlock(const void* p)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if ((a & (kBlock - 1)) || !InRanges(a))
        return -1;
    const ptrdiff_t i = std::lower_bound(g_blocks.begin(), g_blocks.end(), a) - g_blocks.begin();
    AcquireSRWLockShared(&g_lock);
    const bool out = i < static_cast<ptrdiff_t>(g_blocks.size()) && g_blocks[i] == a && g_out[i];
    ReleaseSRWLockShared(&g_lock);
    return out ? i : -1;
}


void* Take()
{
    uintptr_t block = 0;
    uint32_t index = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_free.empty())
    {
        index = g_free.back();
        block = g_blocks[index];
        g_free.pop_back();
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!block)
    {
        g_missed++;
        return nullptr;
    }
    if (!VirtualAlloc(reinterpret_cast<void*>(block), kBlock, MEM_COMMIT, PAGE_READWRITE))
    {
        g_commitFailed++;
        AcquireSRWLockExclusive(&g_lock);
        g_free.push_back(index);
        ReleaseSRWLockExclusive(&g_lock);
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_out[index] = 1;
    ReleaseSRWLockExclusive(&g_lock);
    g_allocs++;
    const int64_t used = ++g_inUse;
    int64_t peak = g_peak.load();
    while (used > peak && !g_peak.compare_exchange_weak(peak, used))
    {
    }
    return reinterpret_cast<void*>(block);
}

void Give(ptrdiff_t index)
{
    AcquireSRWLockExclusive(&g_lock);
    if (!g_out[index])
    {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_out[index] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    VirtualFree(reinterpret_cast<void*>(g_blocks[index]), kBlock, MEM_DECOMMIT);
    AcquireSRWLockExclusive(&g_lock);
    g_free.push_back(static_cast<uint32_t>(index));
    ReleaseSRWLockExclusive(&g_lock);
    g_frees++;
    g_inUse--;
}
} // namespace

bool Init()
{
    if (g_active)
        return !g_blocks.empty();
    wchar_t text[16] = {};
    size_t want = GetEnvironmentVariableW(L"CRYMT_SAFEPOOL_MB", text, 16) ? static_cast<size_t>(_wtoi(text)) : 2048;
    want = std::min<size_t>(want, 3 * 1024) << 20;
    for (uintptr_t at = 0x40000000; at + kPiece <= 0x100000000ull && want; at += kPiece)
    {
        if (at < 0x68000000 && at + kPiece > 0x67800000) // ToolkitPro1042vc80x64.dll (the editor)
            continue;
        if (!VirtualAlloc(reinterpret_cast<void*>(at), kPiece, MEM_RESERVE, PAGE_NOACCESS))
            continue;
        if (!g_ranges.empty() && g_ranges.back().end == at)
            g_ranges.back().end += kPiece;
        else
            g_ranges.push_back({at, at + kPiece});
        for (uintptr_t b = at; b < at + kPiece; b += kBlock)
            g_blocks.push_back(b);
        want -= std::min(want, kPiece);
    }
    g_free.reserve(g_blocks.size());
    g_out.assign(g_blocks.size(), 0);
    for (size_t i = g_blocks.size(); i-- > 0;) // lowest addresses first
        g_free.push_back(static_cast<uint32_t>(i));
    g_active = true; // the functions are used even without a pool (CRYMT_SAFEPOOL_MB=0: the stock behaviour, for tests)
    return !g_blocks.empty();
}

bool Active()
{
    return g_active;
}

void* Malloc(size_t size, size_t& allocated)
{
    if (size == kBlock)
        if (void* p = Take())
        {
            allocated = kBlock;
            return p;
        }
    return Sys().malloc(size, allocated);
}

void* Realloc(void* p, size_t size, size_t& allocated)
{
    const ptrdiff_t i = OurBlock(p);
    if (i < 0)
        return Sys().realloc(p, size, allocated);
    void* n = Sys().malloc(size, allocated); // a pool block grows or shrinks by a copy
    if (n)
    {
        memcpy(n, p, std::min(size, kBlock));
        Give(i);
    }
    return n;
}

size_t Free(void* p)
{
    const ptrdiff_t i = OurBlock(p);
    if (i >= 0)
    {
        Give(i);
        return kBlock;
    }
    return Sys().free(p);
}

size_t GetMemSize(void* p, size_t size)
{
    return OurBlock(p) >= 0 ? kBlock : Sys().size(p, size);
}

void* CrtMalloc(size_t size)
{
    if (size == kBlock)
        if (void* p = Take())
            return p;
    return Sys().crtMalloc(size);
}

void CrtFree(void* p)
{
    const ptrdiff_t i = OurBlock(p);
    if (i >= 0)
        Give(i);
    else
        Sys().crtFree(p);
}

size_t CrtSize(void* p)
{
    return OurBlock(p) >= 0 ? kBlock : Sys().crtSize(p);
}

// A new export table for the executable: its own exports plus the functions above, names sorted (the loader searches
// them by bisection). Export entries are 32-bit offsets from the image base, so the table and small jump stubs to the
// functions go into a page just above the executable.
bool ExportFromExe()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    struct Entry
    {
        std::string name;
        DWORD rva;
    };
    std::vector<Entry> entries;
    std::string module = "Crysis.exe";
    if (dd.VirtualAddress)
    {
        const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dd.VirtualAddress);
        const auto* fns = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
        const auto* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
        const auto* ords = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
        if (ed->Name)
            module = reinterpret_cast<const char*>(base + ed->Name);
        for (DWORD i = 0; i < ed->NumberOfNames; i++)
            entries.push_back({reinterpret_cast<const char*>(base + names[i]), fns[ords[i]]});
    }
    const struct
    {
        const char* name;
        const void* fn;
    } adds[] = {{"CryMalloc", reinterpret_cast<const void*>(&Malloc)},          {"CryRealloc", reinterpret_cast<const void*>(&Realloc)},
                {"CryFree", reinterpret_cast<const void*>(&Free)},              {"CryGetMemSize", reinterpret_cast<const void*>(&GetMemSize)},
                {"CrySystemCrtMalloc", reinterpret_cast<const void*>(&CrtMalloc)}, {"CrySystemCrtFree", reinterpret_cast<const void*>(&CrtFree)},
                {"CrySystemCrtSize", reinterpret_cast<const void*>(&CrtSize)}};
    constexpr size_t kRegion = 0x10000;
    uint8_t* region = nullptr;
    for (uintptr_t at = (base + nt->OptionalHeader.SizeOfImage + 0xFFFF) & ~uintptr_t{0xFFFF}; at < base + 0x7FFF0000 && !region; at += 0x10000)
        region = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), kRegion, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!region)
        return false;
    const auto rva = [base](const void* p) { return static_cast<DWORD>(reinterpret_cast<uintptr_t>(p) - base); };
    uint8_t* stub = region; // jmp [rip+0]; dq target (outside the directory's range: a function inside it is a forwarder)
    for (const auto& a : adds)
    {
        stub[0] = 0xFF;
        stub[1] = 0x25;
        memset(stub + 2, 0, 4);
        const auto target = reinterpret_cast<uint64_t>(a.fn);
        memcpy(stub + 6, &target, 8);
        entries.push_back({a.name, rva(stub)});
        stub += 16;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return strcmp(x.name.c_str(), y.name.c_str()) < 0; });
    const DWORD n = static_cast<DWORD>(entries.size());
    auto* ed = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(region + 0x400);
    auto* fns = reinterpret_cast<DWORD*>(ed + 1);
    auto* names = fns + n;
    auto* ords = reinterpret_cast<WORD*>(names + n);
    char* text = reinterpret_cast<char*>(ords + n + 1);
    memset(ed, 0, sizeof(*ed));
    ed->Base = 1;
    ed->NumberOfFunctions = ed->NumberOfNames = n;
    ed->AddressOfFunctions = rva(fns);
    ed->AddressOfNames = rva(names);
    ed->AddressOfNameOrdinals = rva(ords);
    ed->Name = rva(text);
    memcpy(text, module.c_str(), module.size() + 1);
    text += module.size() + 1;
    for (DWORD i = 0; i < n; i++)
    {
        fns[i] = entries[i].rva;
        ords[i] = static_cast<WORD>(i);
        names[i] = rva(text);
        memcpy(text, entries[i].name.c_str(), entries[i].name.size() + 1);
        text += entries[i].name.size() + 1;
    }
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    VirtualProtect(&dd, sizeof(dd), PAGE_READWRITE, &old);
    dd.VirtualAddress = rva(ed);
    dd.Size = static_cast<DWORD>(reinterpret_cast<uint8_t*>(text) - reinterpret_cast<uint8_t*>(ed));
    VirtualProtect(&dd, sizeof(dd), old, &old);
    return reinterpret_cast<uint8_t*>(GetProcAddress(reinterpret_cast<HMODULE>(base), "CryMalloc")) == region;
}

void WriteStats(FILE* f)
{
    if (!g_active)
        return;
    if (g_ranges.empty())
    {
        fprintf(f, "memory fix (no c1-launcher v8): no pool (CRYMT_SAFEPOOL_MB=0 or nothing could be reserved), 0x80000 requests %lld\n",
                g_missed.load());
        return;
    }
    fprintf(f, "memory fix (no c1-launcher v8): pool %zu MB in %zu pieces from 0x%llx, blocks in use %lld (peak %lld), "
               "handed out %lld, returned %lld, missed the pool %lld, commit failed %lld\n",
            g_blocks.size() * kBlock >> 20, g_ranges.size(), static_cast<unsigned long long>(g_ranges.front().base), g_inUse.load(), g_peak.load(),
            g_allocs.load(), g_frees.load(), g_missed.load(), g_commitFailed.load());
}
} // namespace memfix
