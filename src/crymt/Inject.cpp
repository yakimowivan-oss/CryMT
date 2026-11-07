// Starts Bin64\Crysis.exe suspended, loads CryMT.dll into it, resumes it, and prints its PID.
// Usage: Inject [--wait] [--native] <path to CryMT.dll | none> <game dir> [game args...]
// --wait: return only when the game has exited (for the benchmark scripts). Relative paths are resolved here.
// --native: fullscreen at the primary monitor's current mode (added before the game args, so they can override it).
// "none" instead of the DLL starts the unmodified game the same way (benchmark comparisons).
#include <windows.h>

#include <cstdio>
#include <string>

std::wstring FullPath(const wchar_t* path)
{
    wchar_t full[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(path, MAX_PATH * 2, full, nullptr);
    return n && n < MAX_PATH * 2 ? std::wstring(full, n) : std::wstring(path);
}

int wmain(int argc, wchar_t** argv)
{
    int a = 1;
    bool wait = false, native = false;
    for (; a < argc && wcsncmp(argv[a], L"--", 2) == 0; a++)
    {
        if (wcscmp(argv[a], L"--wait") == 0)
            wait = true;
        else if (wcscmp(argv[a], L"--native") == 0)
            native = true;
        else
            break;
    }
    if (argc < a + 2)
    {
        fprintf(stderr, "usage: Inject [--wait] [--native] <CryMT.dll | none> <game dir> [game args...]\n");
        return 2;
    }
    const bool inject = _wcsicmp(argv[a], L"none") != 0;
    const std::wstring dll = inject ? FullPath(argv[a]) : std::wstring();
    std::wstring game = FullPath(argv[a + 1]);
    while (!game.empty() && (game.back() == L'\\' || game.back() == L'/'))
        game.pop_back();
    const std::wstring bin = game + L"\\Bin64";
    if (inject && GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        fprintf(stderr, "CryMT.dll not found: %ls\n", dll.c_str());
        return 1;
    }
    if (GetFileAttributesW((bin + L"\\Crysis.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        fprintf(stderr, "Bin64\\Crysis.exe not found in %ls\n", game.c_str());
        return 1;
    }
    std::wstring cmd = L"\"" + bin + L"\\Crysis.exe\"";
    if (native)
    {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        cmd += L" +r_Fullscreen 1";
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth && mode.dmPelsHeight)
        {
            cmd += L" +r_Width " + std::to_wstring(mode.dmPelsWidth) + L" +r_Height " + std::to_wstring(mode.dmPelsHeight);
            printf("fullscreen %lux%lu\n", mode.dmPelsWidth, mode.dmPelsHeight);
        }
    }
    for (int i = a + 2; i < argc; i++)
        cmd += std::wstring(L" ") + argv[i];

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, bin.c_str(), &si, &pi))
    {
        fprintf(stderr, "CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    if (!inject)
    {
        ResumeThread(pi.hThread);
        printf("%lu\n", pi.dwProcessId);
        fflush(stdout);
        DWORD code = 0;
        if (wait)
        {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &code);
        }
        return static_cast<int>(code);
    }
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr);
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!t)
    {
        fprintf(stderr, "CreateRemoteThread failed %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    WaitForSingleObject(t, 15000);
    DWORD loaded = 0;
    GetExitCodeThread(t, &loaded);
    if (!loaded)
    {
        fprintf(stderr, "LoadLibrary failed in target\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    ResumeThread(pi.hThread);
    printf("%lu\n", pi.dwProcessId);
    fflush(stdout);
    if (wait)
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        return static_cast<int>(code);
    }
    return 0;
}
