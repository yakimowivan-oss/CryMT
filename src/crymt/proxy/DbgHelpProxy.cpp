// Bin64\dbghelp.dll: loads CryMT at the start of every Crysis.exe launch (Steam, shortcuts, mod .bat files), before
// any engine module, and forwards all of dbghelp to the system copy (DbgHelpStubs.S jumps through g_dbghelp).
// Mode: CryMT\CryMT.ini [CryMT] Mode=off|mt|dx12|dlaa, overridden by a -crymt=<mode> argument; [Env] NAME=VALUE lines
// set extra CryMT switches. When a launcher has already set CRYMT_MODE (Inject.exe, the dev scripts), it controls
// CryMT and the proxy only forwards.
#include <windows.h>

#include <cwchar>
#include <string>

#include "DbgHelpExports.h"

extern "C" {
void* g_dbghelp[kDbgHelpExports];

uintptr_t MissingExport()
{
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}
}

namespace
{
std::wstring ModuleDir(HMODULE module)
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    const std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

// The value of <name><value> on the command line (lower case), or empty.
std::wstring ArgValue(const wchar_t* name)
{
    std::wstring cmd = GetCommandLineW();
    CharLowerBuffW(cmd.data(), static_cast<DWORD>(cmd.size()));
    const size_t at = cmd.find(name);
    if (at == std::wstring::npos)
        return {};
    const size_t start = at + wcslen(name);
    return cmd.substr(start, cmd.find_first_of(L" \t\"", start) - start);
}

// -crymt=<mode> on the command line, else the ini's Mode (mt when there is none), lower case.
std::wstring ChosenMode(const std::wstring& ini)
{
    const std::wstring arg = ArgValue(L"-crymt=");
    if (!arg.empty())
        return arg;
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"CryMT", L"Mode", L"mt", value, 32, ini.c_str());
    CharLowerW(value);
    return value;
}

void LoadCryMT(HMODULE self)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    // The Sandbox Editor only when CryMT's launcher started it with CRYMT_EDITOR=1 in its environment (it loads this
    // proxy into the suspended editor): a plain start of the editor stays stock.
    wchar_t editorMark[4] = {};
    const bool editor = name && (_wcsicmp(name + 1, L"EditorLauncher.exe") == 0 || _wcsicmp(name + 1, L"Editor.exe") == 0) &&
                        GetEnvironmentVariableW(L"CRYMT_EDITOR", editorMark, 4) && wcscmp(editorMark, L"1") == 0;
    if (!name || (!editor && _wcsicmp(name + 1, L"Crysis.exe") != 0)) // not the dedicated server or tools
        return;
    if (GetEnvironmentVariableW(L"CRYMT_MODE", nullptr, 0))
        return;
    const std::wstring bin = ModuleDir(self);
    const std::wstring root = bin.substr(0, bin.find_last_of(L"\\/", bin.size() - 2) + 1);
    const std::wstring dir = root + L"CryMT\\";
    const std::wstring ini = dir + L"CryMT.ini";
    const std::wstring mode = ChosenMode(ini);
    if (mode != L"mt" && mode != L"dx12" && mode != L"dlaa")
        return;
    SetEnvironmentVariableW(L"CRYMT_MODE", L"2");
    SetEnvironmentVariableW(L"CRYMT_LEVEL", L"2");
    SetEnvironmentVariableW(L"CRYMT_FILL", L"1");
    SetEnvironmentVariableW(L"CRYMT_TERRAINOWN", L"1");
    if (mode != L"mt")
    {
        SetEnvironmentVariableW(L"CRYMT_D3D12", L"1");
        SetEnvironmentVariableW(L"CRYMT_D3DSTREAM", L"1");
    }
    // Not in the editor: its viewports are not the swap chain's size, DLSS was skipped there and the camera jitter alone
    // shook the picture (the user, 2026-11-11).
    if (mode == L"dlaa" && !editor)
        SetEnvironmentVariableW(L"CRYMT_DLSS", L"1");
    wchar_t borderless[8] = {};
    GetPrivateProfileStringW(L"CryMT", L"Borderless", L"1", borderless, 8, ini.c_str());
    SetEnvironmentVariableW(L"CRYMT_BORDERLESS", editor ? L"0" : borderless); // the editor keeps its own windows
    wchar_t pause[8] = {};
    GetPrivateProfileStringW(L"CryMT", L"PauseOnFocusLoss", L"1", pause, 8, ini.c_str());
    SetEnvironmentVariableW(L"CRYMT_PAUSEONFOCUS", editor ? L"0" : pause); // focus moves between the editor's panels
    if (editor)
        SetEnvironmentVariableW(L"CRYMT_ASYNCSAVE", L"0"); // the game's save hooks do not cover the editor's saves
    // Console commands CryMT runs from the first frame (CRYMT_STARTCMD, one per frame): what the .cmd launchers pass on
    // the command line, plus the [CVars] lines of the ini ("r_Something=value"), so a plain start of Crysis.exe is the
    // same. A test script's own CRYMT_TESTCMD stays as it is and runs after these (it was overwritten here before).
    // e_gsm_cache 1: far shadow cascades cached (on in the game's High spec, off in Very High; -25% shadow prep on harbor,
    // the user saw no difference). The launcher's option writes "e_gsm_cache=0" into [CVars] to turn it off (applied after).
    std::wstring commands = L"r_DynTexMaxSize 256;e_gsm_cache 1";
    if (mode == L"dlaa" && !editor)
        commands += L";r_UseEdgeAA 0";
    wchar_t cvars[2048] = {};
    GetPrivateProfileSectionW(L"CVars", cvars, 2048, ini.c_str());
    for (wchar_t* p = cvars; *p; p += wcslen(p) + 1)
    {
        std::wstring line = p;
        for (wchar_t& ch : line)
            if (ch == L'=')
                ch = L' ';
        commands += L";" + line;
    }
    SetEnvironmentVariableW(L"CRYMT_STARTCMD", commands.c_str());
    // GI from the ini, or -crymt-gi=0|1 on the command line (the launcher's benchmarks run presets without touching it).
    wchar_t gi[8] = {};
    const std::wstring giArg = ArgValue(L"-crymt-gi=");
    if (!giArg.empty())
        wcsncpy_s(gi, giArg.c_str(), _TRUNCATE);
    else
        GetPrivateProfileStringW(L"CryMT", L"GI", L"0", gi, 8, ini.c_str());
    if (mode != L"mt" && gi[0] && wcscmp(gi, L"0") != 0)
        SetEnvironmentVariableW(L"CRYMT_GI", gi); // GI, D3D12 modes; needs the patched shaders
    wchar_t env[4096] = {};
    GetPrivateProfileSectionW(L"Env", env, 4096, ini.c_str());
    for (wchar_t* p = env; *p; p += wcslen(p) + 1)
        if (wchar_t* eq = wcschr(p, L'='))
        {
            *eq = 0;
            SetEnvironmentVariableW(p, eq + 1);
            *eq = L'=';
        }
    LoadLibraryW((dir + L"CryMT.dll").c_str());
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dbghelp.dll");
        HMODULE real = LoadLibraryW(path);
        for (int i = 0; i < kDbgHelpExports; i++)
        {
            void* fn = real ? reinterpret_cast<void*>(GetProcAddress(real, kDbgHelpNames[i])) : nullptr;
            g_dbghelp[i] = fn ? fn : reinterpret_cast<void*>(&MissingExport);
        }
        LoadCryMT(self);
    }
    return TRUE;
}
