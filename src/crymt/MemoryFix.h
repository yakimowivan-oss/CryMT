// The 64-bit memory fix without c1-launcher v8 (see MemoryFix.cpp).
#pragma once
#include <cstddef>
#include <cstdio>

namespace memfix
{
bool Init();   // turns the functions on and reserves the pool below 4 GB (false: no pool); before the engine modules load
bool Active(); // the functions below are in use
bool ExportFromExe(); // adds them to Crysis.exe's export table in memory (as c1-launcher v8 exports them)

// The engine's memory functions as CrySystem.dll exports them, with 0x80000 blocks from the pool.
void* Malloc(size_t size, size_t& allocated);
void* Realloc(void* p, size_t size, size_t& allocated);
size_t Free(void* p);
size_t GetMemSize(void* p, size_t size);
void* CrtMalloc(size_t size);
void CrtFree(void* p);
size_t CrtSize(void* p);

void WriteStats(FILE* f); // pool size, blocks in use, peak, allocations that missed the pool
} // namespace memfix
