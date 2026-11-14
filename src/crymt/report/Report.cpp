// CryMTReport.exe: started by CryMT.dll with the game's process id. When the game ends abnormally (exit code not 0, a
// new crymt_fault.txt, or "CRASH DETECTED" in Game.log / the editor's Editor.log) it copies the session's logs (and the
// engine's error.log / error.dmp when written) into CryMT\reports\crash_<time>, zips them
// and tells the player what happened. It runs outside the game, so it also sees fast-fail exits and kills that no
// handler inside the process can report, and its window is never hidden behind a fullscreen game.
// Usage: CryMTReport <pid> <CryMT dir\> <game dir\>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
FILETIME g_start;

bool NewerThanStart(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &g_start) > 0;
}

std::string ReadFile(const std::wstring& path)
{
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);
    return text;
}

// The last entry of crymt_fault.txt: the fault line and the first frames.
std::string FaultSummary(const std::string& log)
{
    size_t at = log.rfind("\nfault ");
    if (at == std::string::npos)
        at = log.find("fault ") == 0 ? 0 : std::string::npos;
    if (at == std::string::npos)
        return {};
    std::string out;
    int lines = 0;
    for (size_t i = at + (log[at] == '\n'); i < log.size() && lines < 9; i++)
    {
        out += log[i];
        if (log[i] == '\n')
            lines++;
    }
    return out;
}

const wchar_t* CodeName(DWORD code)
{
    switch (code)
    {
    case 0xC0000005: return L"access violation";
    case 0xC0000409: return L"fast fail (security check or heap corruption)";
    case 0xC0000374: return L"heap corruption";
    case 0xC00000FD: return L"stack overflow";
    case 0xC000001D: return L"illegal instruction";
    case 0xE06D7363: return L"unhandled C++ exception";
    case 0x80000003: return L"breakpoint";
    case 1: return L"terminated (closed from outside or by the crash handler)";
    case 0xFFFFFFFF: return L"terminated from outside (Task Manager or a script)";
    case 0xE0C1C100: return L"abort (c1-launcher)";
    case 0xE0C1C101: return L"pure virtual call (c1-launcher)";
    case 0xE0C1C102: return L"invalid CRT parameter (c1-launcher)";
    case 0xE0C1C103: return L"engine error (CryFatalError, see crymt_fault.txt)";
    default: return L"";
    }
}

std::wstring Widen(const std::string& s)
{
    std::wstring w(s.size(), L'\0');
    for (size_t i = 0; i < s.size(); i++)
        w[i] = static_cast<unsigned char>(s[i]);
    return w;
}

bool RunHidden(std::wstring cmd)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 4)
        return 2;
    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    const std::wstring dir = argv[2], game = argv[3];
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 1;
    FILETIME exitTime, kernel, user;
    GetProcessTimes(process, &g_start, &exitTime, &kernel, &user);
    WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);

    const std::wstring logs = dir + L"logs\\mt\\";
    const bool fault = NewerThanStart(logs + L"crymt_fault.txt");
    const bool hang = NewerThanStart(logs + L"crymt_hang.txt");
    // The Sandbox Editor writes Editor.log, not Game.log (a friend's editor crash of 2026-11-15 came with the game's
    // Game.log of four days before and no editor log): the session's own log, whichever this run wrote.
    const bool editor = NewerThanStart(game + L"Editor.log");
    const wchar_t* logName = editor ? L"Editor.log" : L"Game.log";
    const std::string gameLog = NewerThanStart(game + logName) ? ReadFile(game + logName) : std::string();
    const bool crashLine = gameLog.find("CRASH DETECTED") != std::string::npos;
    if (code == 0 && !fault && !crashLine)
        return 0;

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf_s(stamp, L"crash_%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring reports = dir + L"reports\\";
    const std::wstring folder = reports + stamp + L"\\";
    CreateDirectoryW(reports.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    // crymt_dump.txt: whether the crash minidump (logs\mt\crymt_crash.dmp, left there: large) was written.
    for (const wchar_t* name : {L"crymt_fault.txt", L"crymt_hang.txt", L"crymt_stats.txt", L"crymt_off.txt", L"crymt_focus.txt", L"d3d12.txt",
                                L"d3dlayer.txt", L"d3d10_census.txt", L"sl.log", L"crymt_dump.txt"})
        if (NewerThanStart(logs + name))
            CopyFileW((logs + name).c_str(), (folder + name).c_str(), FALSE);
    // The session's logs only (an old Game.log misled the reading of an editor crash).
    for (const wchar_t* name : {L"Game.log", L"Editor.log"})
        if (NewerThanStart(game + name))
            CopyFileW((game + name).c_str(), (folder + name).c_str(), FALSE);
    // The engine's own crash handler: its call stack (error.log) and dump, where this session wrote them (the game's
    // folder or Bin64); a dump only up to 64 MB (the zip is for sending).
    for (const wchar_t* name : {L"error.log", L"Bin64\\error.log", L"error.dmp", L"Bin64\\error.dmp"})
    {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!NewerThanStart(game + name) || !GetFileAttributesExW((game + name).c_str(), GetFileExInfoStandard, &a) ||
            a.nFileSizeHigh != 0 || a.nFileSizeLow > 64u * 1024 * 1024)
            continue;
        std::wstring flat = name;
        for (wchar_t& ch : flat)
            if (ch == L'\\')
                ch = L'_';
        CopyFileW((game + name).c_str(), (folder + flat).c_str(), FALSE);
    }
    CopyFileW((dir + L"CryMT.ini").c_str(), (folder + L"CryMT.ini").c_str(), FALSE);

    const std::string faultText = fault ? FaultSummary(ReadFile(logs + L"crymt_fault.txt")) : std::string();
    wchar_t head[256];
    swprintf_s(head, L"Exit code 0x%08lX %ls", code, CodeName(code));
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (folder + L"summary.txt").c_str(), L"w") == 0 && f)
        {
            fwprintf(f, L"%ls\n%ls\ncrymt_fault: %d, crymt_hang: %d, CRASH DETECTED in %ls: %d\n\n%hs", head,
                     editor ? L"Sandbox Editor (Editor.log)" : L"game (Game.log)", fault, hang, logName, crashLine, faultText.c_str());
            if (NewerThanStart(logs + L"crymt_dump.txt"))
                fwprintf(f, L"\nCryMT's minidump: CryMT\\logs\\mt\\crymt_crash.dmp (not in the zip: large; send it when asked)\n%hs",
                         ReadFile(logs + L"crymt_dump.txt").c_str());
            // The log's last lines: the crash's own lines are there when the engine wrote them.
            size_t at = gameLog.size();
            for (int lines = 0; at > 0 && lines < 40; lines++)
            {
                const size_t nl = gameLog.rfind('\n', at - 1);
                at = nl == std::string::npos ? 0 : nl;
            }
            std::string tail = gameLog.substr(at);
            tail.erase(std::remove(tail.begin(), tail.end(), '\r'), tail.end()); // the text-mode file adds its own
            if (!tail.empty())
                fwprintf(f, L"\n--- the end of %ls ---\n%hs\n", logName, tail.c_str());
            fclose(f);
        }
    }
    const std::wstring zip = reports + stamp + L".zip";
    const bool zipped = RunHidden(L"tar.exe -a -c -f \"" + zip + L"\" -C \"" + folder + L".\" *");

    const bool russian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    std::wstring text = editor ? (russian ? L"Редактор (Sandbox) завершился с ошибкой.\n\n" : L"The Sandbox Editor closed unexpectedly.\n\n")
                               : (russian ? L"Crysis завершился с ошибкой.\n\n" : L"Crysis closed unexpectedly.\n\n");
    text += head;
    text += L"\n";
    if (!faultText.empty())
        text += L"\n" + Widen(faultText);
    else if (hang)
        text += russian ? L"\nИгра перед этим зависла (crymt_hang.txt).\n" : L"\nThe game had stopped responding (crymt_hang.txt).\n";
    text += russian ? L"\nОтчёт сохранён:\n" : L"\nThe report was saved to:\n";
    text += zipped ? zip : folder;
    text += russian ? L"\n\nПришлите его автору CryMT (Discord или GitHub). Открыть папку с отчётом?"
                    : L"\n\nPlease send it to the CryMT author (Discord or GitHub). Open the report folder?";
    if (MessageBoxW(nullptr, text.c_str(), L"CryMT", MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST) == IDYES)
    {
        const std::wstring args = L"/select,\"" + (zipped ? zip : folder) + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
