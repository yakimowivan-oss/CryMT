#include <windows.h>

#include "Paths.h"

namespace
{
std::wstring ModuleDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

std::string Narrow(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The folder (created once) as a narrow path with a trailing backslash.
const std::string& Folder(const wchar_t* sub)
{
    static std::string logs, play;
    std::string& slot = sub[5] == L'm' ? logs : play; // "logs\\mt" or "logs\\play"
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    if (slot.empty())
    {
        CreateDirectoryW((CryMTDirW() + L"logs").c_str(), nullptr);
        CreateDirectoryW((CryMTDirW() + sub).c_str(), nullptr);
        slot = Narrow(CryMTDirW() + sub) + "\\";
    }
    ReleaseSRWLockExclusive(&lock);
    return slot;
}
} // namespace

const std::wstring& CryMTDirW()
{
    static const std::wstring dir = ModuleDir();
    return dir;
}

std::string CryMTLog(const char* name)
{
    return Folder(L"logs\\mt") + name;
}

std::string CryMTCapture(const char* name)
{
    return Folder(L"logs\\play") + name;
}

std::wstring CryMTCacheW(const wchar_t* name)
{
    CreateDirectoryW((CryMTDirW() + L"cache").c_str(), nullptr);
    return CryMTDirW() + L"cache\\" + name;
}
