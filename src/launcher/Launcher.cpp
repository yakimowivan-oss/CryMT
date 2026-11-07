// CryMTLauncher.exe: the CryMT launcher's window. A window without the system frame (the page draws the title bar, its
// minimise / close buttons and moves the window by its header) holding the HTML UI (ui\index.html) in WebView2.
// Lives in <game>\CryMT; the page does the settings, this file only touches files and processes for it:
//   page -> window (strings): load | save\n<CryMT.ini text> | play\n<gi 0|1>\n<arguments>\n<minimize|close|stay> |
//                             drag | min | close | open\n<game|crymt|logs>
//                             fetch\n<id>\n<url> | update-manifest\n<url>\n<signature url> | update-install\n<zip url> |
//                             rollback | verify
//   window -> page (JSON):    {type: state, ini, exists, reason, status{...}} | {type: saved, ini} | {type: played, ok, error}
//                             {type: fetched, id, ok, status, body} | {type: manifest, ok, error, text} |
//                             {type: progress, stage, done, total} | {type: installed | rolledback, ok, error, restart} |
//                             {type: verified, ok, checked, wrong, bad[]}
// Updates (see ParseManifest below): the page finds the release on GitHub, the window downloads its manifest and checks
// the signature against the key compiled in (UpdateKey.h, ECDSA P-256), then the package's files against the manifest.
// Command line: --ui=<folder> (default <exe folder>\ui; for working on the page in place), --game=<folder> (default the
// exe folder's parent), --dev (F5, F12 and the context menu work).
#define _WIN32_WINNT 0x0A00 // Windows 10: per-monitor DPI
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#ifndef DECLSPEC_XFGVIRT
#define DECLSPEC_XFGVIRT(base, func)
#endif
#include "WebView2.h"
#include "UpdateKey.h"

namespace
{
const wchar_t* const kClass = L"CryMTLauncher";
const int kWidth = 1280, kHeight = 800; // at 96 DPI
const COLORREF kBack = RGB(3, 7, 6);    // the page's --bg0, shown before it paints
const UINT WM_HOSTJSON = WM_APP + 1;    // lParam: std::wstring* JSON for the page, from a worker thread

HWND g_wnd;
ICoreWebView2Controller* g_ctrl;
ICoreWebView2* g_view;
std::wstring g_dir, g_game, g_ui; // with trailing backslashes
std::wstring g_page;               // --page=<suffix>: appended to the page's address (tests: #news?auto=update)
bool g_dev;
bool g_backedUp;
HANDLE g_once;                 // the one-launcher mutex (released before a restart)
std::atomic<bool> g_working;   // an update, rollback, check or download runs on a worker thread
std::mutex g_manifestLock;
std::string g_manifest;        // the last manifest whose signature checked out
FILETIME g_iniTime; // CryMT.ini's write time when the page last read or wrote it

// COM handler for WebView2's callbacks: one Invoke from a lambda.
template <class I, class... A>
class Handler final : public I
{
    const IID& m_iid;
    std::function<HRESULT(A...)> m_fn;
    LONG m_ref = 1;

public:
    Handler(const IID& iid, std::function<HRESULT(A...)> fn) : m_iid(iid), m_fn(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid))
        {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG r = InterlockedDecrement(&m_ref);
        if (!r)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return m_fn(a...); }
};

std::wstring Utf16(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}
std::string Utf8(const std::wstring& w)
{
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    return s;
}
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::string ReadAll(const std::wstring& path)
{
    std::string data;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return data;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!data.empty() && !ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr))
        got = 0;
    data.resize(got);
    CloseHandle(f);
    return data;
}
bool WriteAll(const std::wstring& path, const std::string& data)
{
    const std::wstring tmp = path + L".tmp";
    const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size();
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
FILETIME WriteTime(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
    return a.ftLastWriteTime;
}
bool SameTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

std::wstring Json(const std::wstring& s)
{
    std::wstring o = L"\"";
    for (const wchar_t c : s)
    {
        if (c == L'"' || c == L'\\')
            o += L'\\', o += c;
        else if (c == L'\n')
            o += L"\\n";
        else if (c == L'\r')
            o += L"\\r";
        else if (c == L'\t')
            o += L"\\t";
        else if (c < 0x20)
        {
            wchar_t b[8];
            swprintf(b, 8, L"\\u%04x", c);
            o += b;
        }
        else
            o += c;
    }
    return o + L"\"";
}
const wchar_t* Bool(bool b) { return b ? L"true" : L"false"; }

void Send(const std::wstring& json)
{
    if (g_view)
        g_view->PostWebMessageAsJson(json.c_str());
}
// From a worker thread: hand the JSON to the window's thread.
void SendLater(const std::wstring& json) { PostMessageW(g_wnd, WM_HOSTJSON, 0, reinterpret_cast<LPARAM>(new std::wstring(json))); }

std::wstring IniPath() { return g_dir + L"CryMT.ini"; }

bool InstalledInfo(std::string& version, std::string& date); // CryMTersion.txt (below, with the updates)

// The game's state for the status bar: build, c1-launcher, the proxy, why CryMT stayed off last time.
std::wstring Status()
{
    std::wstring build = L"?", launcher = L"нет c1-launcher";
    bool buildOk = false, launcherOk = false;
    DWORD dummy = 0;
    const std::wstring sys = g_game + L"Bin64\\CrySystem.dll";
    if (const DWORD size = GetFileVersionInfoSizeW(sys.c_str(), &dummy))
    {
        std::vector<BYTE> info(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(sys.c_str(), 0, size, info.data()) && VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) && fixed)
        {
            build = std::to_wstring(LOWORD(fixed->dwFileVersionLS)); // 6156 = patch 1.2.1 (the files say 1.1.1.6156)
            buildOk = LOWORD(fixed->dwFileVersionLS) == 6156;
        }
    }
    const std::wstring exe = g_game + L"Bin64\\Crysis.exe";
    if (const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy))
    {
        std::vector<BYTE> info(size);
        wchar_t* product = nullptr;
        wchar_t* version = nullptr;
        UINT len = 0;
        WORD* lang = nullptr;
        if (GetFileVersionInfoW(exe.c_str(), 0, size, info.data()) &&
            VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&lang), &len) && len >= 4)
        {
            wchar_t key[64];
            swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\ProductName", lang[0], lang[1]);
            VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&product), &len);
            swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileVersion", lang[0], lang[1]);
            VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&version), &len);
        }
        if (product && wcsstr(product, L"C1-Launcher"))
        {
            launcher = std::wstring(L"c1-launcher ") + (version ? version : L"");
            launcherOk = version && (wcsstr(version, L"v8") || wcsstr(version, L"v9"));
            if (!launcherOk)
                launcher += L" — нужен v8";
        }
    }
    const std::string off = ReadAll(g_dir + L"logs\\mt\\crymt_off.txt");
    std::string version, date;
    InstalledInfo(version, date);
    return L"{\"installed\":" + Json(Utf16(version)) + L",\"installedDate\":" + Json(Utf16(date)) +
           L",\"game\":" + Json(g_game) + L",\"build\":" + Json(build) + L",\"buildOk\":" + Bool(buildOk) + L",\"launcher\":" + Json(launcher) +
           L",\"launcherOk\":" + Bool(launcherOk) + L",\"proxy\":" + Bool(Exists(g_game + L"Bin64\\dbghelp.dll")) + L",\"lastOff\":" + Json(Utf16(off)) + L"}";
}

void SendState(const wchar_t* reason)
{
    const std::wstring ini = IniPath();
    const bool exists = Exists(ini);
    g_iniTime = WriteTime(ini);
    Send(L"{\"type\":\"state\",\"ini\":" + Json(Utf16(ReadAll(ini))) + L",\"exists\":" + Bool(exists) + L",\"reason\":" + Json(reason) +
         L",\"status\":" + Status() + L"}");
}

void Save(const std::wstring& text)
{
    const std::wstring ini = IniPath();
    // Edited by hand (or by another tool) since the page read it: the page reads it again instead of overwriting.
    if (Exists(ini) && !SameTime(WriteTime(ini), g_iniTime))
    {
        SendState(L"external");
        return;
    }
    const std::string data = Utf8(text);
    if (data == ReadAll(ini))
        return;
    if (!g_backedUp && Exists(ini)) // the file as it was before this launcher session first changed it
        g_backedUp = CopyFileW(ini.c_str(), (g_dir + L"cache\\CryMT.ini.bak").c_str(), FALSE) || true;
    if (!WriteAll(ini, data))
    {
        Send(L"{\"type\":\"error\",\"error\":" + Json(L"не удалось записать " + ini) + L"}");
        return;
    }
    g_iniTime = WriteTime(ini);
    Send(L"{\"type\":\"saved\",\"ini\":" + Json(text) + L"}");
}

DWORD RunWait(std::wstring cmd)
{
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_dir.c_str(), &si, &pi))
        return GetLastError() | 0x80000000u;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// Shaders (CryMTSettings.exe installs or removes the GI pak; an unchanged pak is not rewritten), then the game.
void Play(bool gi, const std::wstring& args, const std::wstring& after)
{
    std::thread([gi, args, after] {
        std::wstring error;
        const std::wstring settings = g_dir + L"CryMTSettings.exe";
        if (!Exists(settings))
            error = L"нет " + settings;
        else if (const DWORD code = RunWait(L"\"" + settings + L"\" " + (gi ? L"--gi-install" : L"--gi-remove")))
            error = std::wstring(gi ? L"шейдеры GI не установились" : L"шейдеры GI не удалились") + L" (код " + std::to_wstring(code) + L")";
        if (error.empty())
        {
            const std::wstring bin = g_game + L"Bin64";
            const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", (bin + L"\\Crysis.exe").c_str(), args.c_str(), bin.c_str(), SW_SHOWNORMAL));
            if (r <= 32)
                error = L"Bin64\\Crysis.exe не запустился (" + std::to_wstring(r) + L")";
        }
        SendLater(L"{\"type\":\"played\",\"ok\":" + std::wstring(Bool(error.empty())) + L",\"error\":" + Json(error) + L",\"after\":" +
                  Json(error.empty() ? after : L"stay") + L"}");
    }).detach();
}

// ---------------------------------------------------------------------------------------------------------------------
// Updates. A release (GitHub, CryMT-Releases) carries CryMT-v<N>.zip, manifest.txt and manifest.sig:
//   CryMT <version>
//   date <yyyy-mm-dd>
//   file <path from the game folder, '/'> <size> <sha256>     replaced by an update
//   keep <path> <size> <sha256>                               installed only when missing (the player's own copy stays)
// manifest.sig = ECDSA P-256 over the manifest's SHA-256 (CryMTSign). The installed manifest is CryMT\version.txt.
// Before files are replaced the old ones go to CryMT\backup\<version>\ (files.txt lists them and the new ones), so one
// step back is possible. CryMT.ini is never in a package: the player's settings stay.
// ---------------------------------------------------------------------------------------------------------------------
struct ManifestFile
{
    bool keep = false;
    std::string path;
    uint64_t size = 0;
    std::string sha;
};
struct Manifest
{
    std::string version, date;
    std::vector<ManifestFile> files;
};

bool ParseManifest(const std::string& text, Manifest& m)
{
    size_t at = 0;
    bool first = true;
    while (at < text.size())
    {
        const size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        char a[64] = {}, b[512] = {}, c[512] = {};
        unsigned long long size = 0;
        if (first)
        {
            if (sscanf(line.c_str(), "%63s %63s", a, c) != 2 || strcmp(a, "CryMT") != 0)
                return false;
            m.version = c;
            first = false;
        }
        else if (sscanf(line.c_str(), "date %63s", a) == 1)
            m.date = a;
        else if (sscanf(line.c_str(), "%63s %511s %llu %511s", a, b, &size, c) == 4 && (!strcmp(a, "file") || !strcmp(a, "keep")))
        {
            const std::string path = b;
            if (path.find("..") != std::string::npos || path.find(':') != std::string::npos || path[0] == '/' || path[0] == '\\' || strlen(c) != 64)
                return false;
            m.files.push_back({!strcmp(a, "keep"), path, size, c});
        }
    }
    return !first && !m.files.empty();
}

std::wstring Backslashes(std::wstring w)
{
    for (wchar_t& c : w)
        if (c == L'/')
            c = L'\\';
    return w;
}

std::wstring GamePath(const std::string& path) { return Backslashes(g_game + Utf16(path)); }

std::string Sha256Hex(const std::string& data)
{
    unsigned char hash[32];
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    const bool ok = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash, 32) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok)
        return {};
    std::string hex;
    char two[3];
    for (const unsigned char x : hash)
    {
        snprintf(two, 3, "%02x", x);
        hex += two;
    }
    return hex;
}

bool SignatureOk(const std::string& data, const std::string& sig)
{
    if (sig.size() != 64)
        return false;
    unsigned char hash[32];
    BCRYPT_ALG_HANDLE sha = nullptr, ecdsa = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    const bool ok = BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                    BCryptHash(sha, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash, 32) == 0 &&
                    BCryptOpenAlgorithmProvider(&ecdsa, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
                    BCryptImportKeyPair(ecdsa, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, const_cast<PUCHAR>(kUpdateKey), sizeof(kUpdateKey), 0) == 0 &&
                    BCryptVerifySignature(key, nullptr, hash, 32, reinterpret_cast<PUCHAR>(const_cast<char*>(sig.data())), 64, 0) == 0;
    if (key)
        BCryptDestroyKey(key);
    if (ecdsa)
        BCryptCloseAlgorithmProvider(ecdsa, 0);
    if (sha)
        BCryptCloseAlgorithmProvider(sha, 0);
    return ok;
}

// HTTPS GET with WinHTTP (redirects followed: GitHub's asset links go to its file storage).
bool HttpGet(const std::wstring& url, std::string& body, DWORD& status, std::wstring& error,
             const std::function<void(uint64_t, uint64_t)>& progress = {})
{
    status = 0;
    body.clear();
    wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
    {
        error = L"не https-адрес";
        return false;
    }
    const HINTERNET session = WinHttpOpen(L"CryMT-Launcher/13", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        error = L"WinHTTP не запустился";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    const HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    const HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", (std::wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                      : nullptr;
    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok)
    {
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        uint64_t total = 0;
        wchar_t length[32] = {};
        size = sizeof(length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &size, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(length, nullptr, 10);
        uint64_t lastReport = 0;
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail))
            {
                ok = false;
                break;
            }
            if (!avail)
                break;
            const size_t at = body.size();
            body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(request, &body[at], avail, &got))
            {
                ok = false;
                break;
            }
            body.resize(at + got);
            if (progress && body.size() - lastReport >= (1u << 20))
            {
                lastReport = body.size();
                progress(body.size(), total);
            }
        }
        if (progress)
            progress(body.size(), total);
    }
    if (!ok)
        error = L"сеть: ошибка " + std::to_wstring(GetLastError());
    else if (status != 200)
    {
        error = L"сервер ответил " + std::to_wstring(status);
        ok = false;
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// Crysis.exe running from this game folder (its files cannot be replaced then).
bool GameRunning()
{
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool running = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !running; more = Process32NextW(snap, &pe))
    {
        if (_wcsicmp(pe.szExeFile, L"Crysis.exe") != 0)
            continue;
        if (const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID))
        {
            wchar_t image[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, image, &n) && _wcsnicmp(image, g_game.c_str(), g_game.size()) == 0)
                running = true;
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
    return running;
}

bool MakeDirFor(const std::wstring& file)
{
    const std::wstring dir = file.substr(0, file.find_last_of(L'\\'));
    const int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

// A file in use (this launcher, its WebView2Loader.dll) is renamed to .old first; the next start deletes it.
bool PutFile(const std::wstring& from, const std::wstring& to)
{
    if (!MakeDirFor(to))
        return false;
    if (CopyFileW(from.c_str(), to.c_str(), FALSE))
        return true;
    const std::wstring old = to + L".old";
    DeleteFileW(old.c_str());
    return MoveFileExW(to.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && CopyFileW(from.c_str(), to.c_str(), FALSE);
}

bool LauncherFile(const std::string& path)
{
    return _stricmp(path.c_str(), "CryMT/CryMTLauncher.exe") == 0 || _stricmp(path.c_str(), "CryMT/WebView2Loader.dll") == 0;
}

bool InstalledInfo(std::string& version, std::string& date)
{
    Manifest m;
    if (!ParseManifest(ReadAll(g_dir + L"version.txt"), m))
        return false;
    version = m.version;
    date = m.date;
    return true;
}

std::string InstalledVersion()
{
    Manifest m;
    return ParseManifest(ReadAll(g_dir + L"version.txt"), m) ? m.version : std::string();
}

void RemoveTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW f;
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
                continue;
            const std::wstring p = dir + L"\\" + f.cFileName;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(p);
            else
            {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

DWORD RunWait(std::wstring cmd);

// On a worker thread, one at a time.
bool StartWork(const wchar_t* kind, std::function<void()> work)
{
    bool expected = false;
    if (!g_working.compare_exchange_strong(expected, true))
    {
        SendLater(L"{\"type\":\"error\",\"error\":" + Json(std::wstring(L"лаунчер занят: ") + kind) + L"}");
        return false;
    }
    std::thread([work] {
        work();
        g_working = false;
    }).detach();
    return true;
}

void FetchText(const std::wstring& id, const std::wstring& url)
{
    StartWork(L"запрос", [id, url] {
        std::string body;
        DWORD status = 0;
        std::wstring error;
        const bool ok = HttpGet(url, body, status, error);
        SendLater(L"{\"type\":\"fetched\",\"id\":" + Json(id) + L",\"ok\":" + Bool(ok) + L",\"status\":" + std::to_wstring(status) +
                  L",\"error\":" + Json(error) + L",\"body\":" + Json(Utf16(body)) + L"}");
    });
}

void CheckManifest(const std::wstring& url, const std::wstring& sigUrl)
{
    StartWork(L"проверка обновления", [url, sigUrl] {
        std::string text, sig;
        DWORD status = 0;
        std::wstring error;
        Manifest m;
        bool ok = HttpGet(url, text, status, error) && HttpGet(sigUrl, sig, status, error);
        if (ok && !SignatureOk(text, sig))
        {
            ok = false;
            error = L"подпись обновления не совпала с ключом CryMT — не устанавливаю";
        }
        else if (ok && !ParseManifest(text, m))
        {
            ok = false;
            error = L"список файлов обновления не читается";
        }
        if (ok)
        {
            std::lock_guard<std::mutex> guard(g_manifestLock);
            g_manifest = text;
        }
        SendLater(L"{\"type\":\"manifest\",\"ok\":" + std::wstring(Bool(ok)) + L",\"error\":" + Json(error) + L",\"text\":" +
                  Json(ok ? Utf16(text) : L"") + L"}");
    });
}

void Progress(const wchar_t* stage, uint64_t done, uint64_t total)
{
    SendLater(L"{\"type\":\"progress\",\"stage\":" + Json(stage) + L",\"done\":" + std::to_wstring(done) + L",\"total\":" + std::to_wstring(total) + L"}");
}

void Install(const std::wstring& zipUrl)
{
    StartWork(L"обновление", [zipUrl] {
        std::string text;
        {
            std::lock_guard<std::mutex> guard(g_manifestLock);
            text = g_manifest;
        }
        Manifest m;
        std::wstring error;
        bool restart = false;
        const auto done = [&](bool ok) {
            SendLater(L"{\"type\":\"installed\",\"ok\":" + std::wstring(Bool(ok)) + L",\"error\":" + Json(error) + L",\"restart\":" +
                      Bool(ok && restart) + L",\"version\":" + Json(Utf16(m.version)) + L"}");
        };
        if (!ParseManifest(text, m))
        {
            error = L"нет проверенного списка файлов";
            return done(false);
        }
        const std::wstring stage = g_dir + L"cache\\update\\" + Utf16(m.version);
        RemoveTree(stage);
        SHCreateDirectoryExW(nullptr, (stage + L"\\files").c_str(), nullptr);
        std::string zip;
        DWORD status = 0;
        if (!HttpGet(zipUrl, zip, status, error, [](uint64_t d, uint64_t t) { Progress(L"download", d, t); }))
            return done(false);
        if (!WriteAll(stage + L"\\package.zip", zip))
        {
            error = L"не удалось сохранить архив";
            return done(false);
        }
        zip.clear();
        Progress(L"unpack", 0, 0);
        if (RunWait(L"tar.exe -xf \"" + stage + L"\\package.zip\" -C \"" + stage + L"\\files\"") != 0)
        {
            error = L"архив не распаковался (tar.exe)";
            return done(false);
        }
        // Every file of the package against the signed manifest before anything in the game folder changes.
        uint64_t checked = 0;
        for (const ManifestFile& f : m.files)
        {
            const std::string data = ReadAll(Backslashes(stage + L"\\files\\" + Utf16(f.path)));
            if (data.size() != f.size || Sha256Hex(data) != f.sha)
            {
                error = L"файл не совпал со списком: " + Utf16(f.path);
                return done(false);
            }
            Progress(L"check", ++checked, m.files.size());
        }
        if (GameRunning())
        {
            error = L"игра запущена — закройте её и обновите снова";
            return done(false);
        }
        // The files this update touches, as they are now, go to backup\<installed version>.
        const std::string before = InstalledVersion().empty() ? "before-" + m.version : InstalledVersion();
        const std::wstring backup = g_dir + L"backup\\" + Utf16(before);
        RemoveTree(backup);
        SHCreateDirectoryExW(nullptr, backup.c_str(), nullptr);
        std::string list;
        for (const ManifestFile& f : m.files)
        {
            const std::wstring target = GamePath(f.path);
            const bool exists = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (f.keep && exists)
                continue;
            if (exists)
            {
                const std::wstring copy = Backslashes(backup + L"\\" + Utf16(f.path));
                if (!MakeDirFor(copy) || !CopyFileW(target.c_str(), copy.c_str(), FALSE))
                {
                    error = L"не удалось сохранить копию " + Utf16(f.path);
                    return done(false);
                }
                list += "old " + f.path + "\n";
            }
            else
                list += "new " + f.path + "\n";
        }
        if (GetFileAttributesW((g_dir + L"version.txt").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            CopyFileW((g_dir + L"version.txt").c_str(), (backup + L"\\version.txt").c_str(), FALSE);
            list += "version\n";
        }
        WriteAll(backup + L"\\files.txt", list);
        uint64_t put = 0;
        for (const ManifestFile& f : m.files)
        {
            const std::wstring target = GamePath(f.path);
            if (f.keep && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
                continue;
            if (Sha256Hex(ReadAll(target)) != f.sha)
            {
                if (!PutFile(Backslashes(stage + L"\\files\\" + Utf16(f.path)), target))
                {
                    error = L"не удалось заменить " + Utf16(f.path) + L" (вернуть прежнее: «Откатить»)";
                    return done(false);
                }
                restart = restart || LauncherFile(f.path);
            }
            Progress(L"install", ++put, m.files.size());
        }
        WriteAll(g_dir + L"version.txt", text);
        RemoveTree(stage);
        done(true);
    });
}

// One step back: the newest backup\<version> is put back (files that were new are removed).
void Rollback()
{
    StartWork(L"откат", [] {
        std::wstring error, name;
        bool restart = false;
        const auto done = [&](bool ok) {
            SendLater(L"{\"type\":\"rolledback\",\"ok\":" + std::wstring(Bool(ok)) + L",\"error\":" + Json(error) + L",\"restart\":" +
                      Bool(ok && restart) + L",\"version\":" + Json(name) + L"}");
        };
        WIN32_FIND_DATAW f;
        FILETIME newest{};
        const HANDLE h = FindFirstFileW((g_dir + L"backup\\*").c_str(), &f);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && f.cFileName[0] != L'.' && CompareFileTime(&f.ftLastWriteTime, &newest) > 0)
                {
                    newest = f.ftLastWriteTime;
                    name = f.cFileName;
                }
            while (FindNextFileW(h, &f));
            FindClose(h);
        }
        const std::wstring backup = g_dir + L"backup\\" + name;
        const std::string list = name.empty() ? std::string() : ReadAll(backup + L"\\files.txt");
        if (list.empty())
        {
            error = L"нет сохранённой версии для отката";
            return done(false);
        }
        if (GameRunning())
        {
            error = L"игра запущена — закройте её";
            return done(false);
        }
        bool hadVersion = false;
        for (size_t at = 0; at < list.size();)
        {
            const size_t end = list.find('\n', at);
            const std::string line = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? list.size() : end + 1;
            if (line == "version")
                hadVersion = true;
            else if (line.size() > 4)
            {
                const std::string path = line.substr(4);
                const std::wstring target = GamePath(path);
                if (line.compare(0, 4, "old ") == 0)
                {
                    if (!PutFile(Backslashes(backup + L"\\" + Utf16(path)), target))
                    {
                        error = L"не удалось вернуть " + Utf16(path);
                        return done(false);
                    }
                }
                else if (!DeleteFileW(target.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                    MoveFileExW(target.c_str(), (target + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
                restart = restart || LauncherFile(path);
            }
        }
        if (hadVersion)
            CopyFileW((backup + L"\\version.txt").c_str(), (g_dir + L"version.txt").c_str(), FALSE);
        else
            DeleteFileW((g_dir + L"version.txt").c_str());
        RemoveTree(backup);
        done(true);
    });
}

// The installed files against CryMT\version.txt.
void VerifyInstalled()
{
    StartWork(L"проверка файлов", [] {
        Manifest m;
        std::wstring bad;
        int checked = 0, wrong = 0;
        const bool ok = ParseManifest(ReadAll(g_dir + L"version.txt"), m);
        for (const ManifestFile& f : m.files)
        {
            if (f.keep)
                continue;
            checked++;
            const std::string data = ReadAll(GamePath(f.path));
            if (data.size() != f.size || Sha256Hex(data) != f.sha)
            {
                wrong++;
                if (wrong <= 8)
                    bad += (bad.empty() ? L"" : L",") + Json(Utf16(f.path));
            }
        }
        SendLater(L"{\"type\":\"verified\",\"ok\":" + std::wstring(Bool(ok)) + L",\"checked\":" + std::to_wstring(checked) + L",\"wrong\":" +
                  std::to_wstring(wrong) + L",\"bad\":[" + bad + L"]}");
    });
}

void Restart()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring args = PathGetArgsW(GetCommandLineW());
    if (args.find(L"--restarted") == std::wstring::npos)
        args += L" --restarted";
    if (g_once)
    {
        CloseHandle(g_once);
        g_once = nullptr;
    }
    ShellExecuteW(nullptr, L"open", self, args.c_str(), g_dir.c_str(), SW_SHOWNORMAL);
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

void OnMessage(const std::wstring& m)
{
    const size_t nl = m.find(L'\n');
    const std::wstring cmd = m.substr(0, nl), rest = nl == std::wstring::npos ? L"" : m.substr(nl + 1);
    if (cmd == L"drag")
    {
        ReleaseCapture();
        SendMessageW(g_wnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    else if (cmd == L"min")
        ShowWindow(g_wnd, SW_MINIMIZE);
    else if (cmd == L"close")
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    else if (cmd == L"load")
        SendState(L"load");
    else if (cmd == L"save")
        Save(rest);
    else if (cmd == L"play")
    {
        std::vector<std::wstring> part;
        for (size_t at = 0;;)
        {
            const size_t end = rest.find(L'\n', at);
            part.push_back(rest.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at));
            if (end == std::wstring::npos)
                break;
            at = end + 1;
        }
        part.resize(3);
        Play(part[0] == L"1", part[1], part[2]);
    }
    else if (cmd == L"fetch")
    {
        const size_t nl2 = rest.find(L'\n');
        if (nl2 != std::wstring::npos)
            FetchText(rest.substr(0, nl2), rest.substr(nl2 + 1));
    }
    else if (cmd == L"update-manifest")
    {
        const size_t nl2 = rest.find(L'\n');
        if (nl2 != std::wstring::npos)
            CheckManifest(rest.substr(0, nl2), rest.substr(nl2 + 1));
    }
    else if (cmd == L"update-install")
        Install(rest);
    else if (cmd == L"rollback")
        Rollback();
    else if (cmd == L"verify")
        VerifyInstalled();
    else if (cmd == L"open")
    {
        const std::wstring path = rest == L"game" ? g_game : rest == L"logs" ? g_dir + L"logs" : g_dir;
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void CreateView()
{
    const HMODULE loader = LoadLibraryW((g_dir + L"WebView2Loader.dll").c_str());
    using Create = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    const auto create = loader ? reinterpret_cast<Create>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")) : nullptr;
    const auto fail = [](const std::wstring& what) {
        MessageBoxW(g_wnd, (what + L"\n\nНужен Microsoft Edge WebView2 Runtime (есть в Windows 10 и 11 с обновлениями):\n"
                                   L"https://developer.microsoft.com/microsoft-edge/webview2/").c_str(),
                    L"CryMT", MB_ICONERROR);
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    };
    if (!create)
    {
        fail(L"Не найден WebView2Loader.dll рядом с лаунчером.");
        return;
    }
    const std::wstring data = g_dir + L"cache\\launcher";
    const HRESULT hr = create(nullptr, data.c_str(), nullptr,
        new Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, [fail](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env)
                {
                    fail(L"WebView2 не запустился (0x" + std::to_wstring(static_cast<unsigned long>(hr)) + L").");
                    return S_OK;
                }
                return env->CreateCoreWebView2Controller(g_wnd,
                    new Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                        IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(hr) || !ctrl)
                                return S_OK;
                            g_ctrl = ctrl;
                            g_ctrl->AddRef();
                            g_ctrl->get_CoreWebView2(&g_view);
                            ICoreWebView2Controller2* ctrl2 = nullptr;
                            if (SUCCEEDED(g_ctrl->QueryInterface(IID_ICoreWebView2Controller2, reinterpret_cast<void**>(&ctrl2))))
                            {
                                ctrl2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, GetRValue(kBack), GetGValue(kBack), GetBValue(kBack)});
                                ctrl2->Release();
                            }
                            RECT r;
                            GetClientRect(g_wnd, &r);
                            g_ctrl->put_Bounds(r);
                            ICoreWebView2Settings* s = nullptr;
                            if (SUCCEEDED(g_view->get_Settings(&s)))
                            {
                                s->put_AreDefaultContextMenusEnabled(g_dev);
                                s->put_AreDevToolsEnabled(g_dev);
                                s->put_IsStatusBarEnabled(FALSE);
                                s->put_IsZoomControlEnabled(FALSE);
                                ICoreWebView2Settings3* s3 = nullptr;
                                if (SUCCEEDED(s->QueryInterface(IID_ICoreWebView2Settings3, reinterpret_cast<void**>(&s3))))
                                {
                                    s3->put_AreBrowserAcceleratorKeysEnabled(g_dev);
                                    s3->Release();
                                }
                                s->Release();
                            }
                            EventRegistrationToken token;
                            g_view->add_WebMessageReceived(
                                new Handler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
                                    IID_ICoreWebView2WebMessageReceivedEventHandler,
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR text = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&text)) && text)
                                        {
                                            OnMessage(text);
                                            CoTaskMemFree(text);
                                        }
                                        return S_OK;
                                    }),
                                &token);
                            // The page's folder as https://crymt.launcher/ (the page and its scripts load like a site).
                            ICoreWebView2_3* view3 = nullptr;
                            if (SUCCEEDED(g_view->QueryInterface(IID_ICoreWebView2_3, reinterpret_cast<void**>(&view3))))
                            {
                                view3->SetVirtualHostNameToFolderMapping(L"crymt.launcher", g_ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                view3->Release();
                                g_view->Navigate((L"https://crymt.launcher/index.html" + g_page).c_str());
                            }
                            else
                                g_view->Navigate((L"file:///" + g_ui + L"index.html").c_str());
                            g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                            return S_OK;
                        }));
            }));
    if (FAILED(hr))
        fail(L"WebView2 не запустился (0x" + std::to_wstring(static_cast<unsigned long>(hr)) + L").");
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCCALCSIZE: // no system frame: the client area is the whole window
        if (wp)
            return 0;
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_GETMINMAXINFO: // a fixed size (at this monitor's DPI)
    {
        const UINT dpi = GetDpiForWindow(wnd);
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = mm->ptMaxTrackSize = {MulDiv(kWidth, dpi, 96), MulDiv(kHeight, dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED:
    {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        if (g_ctrl)
        {
            g_ctrl->put_IsVisible(wp != SIZE_MINIMIZED);
            RECT r;
            GetClientRect(wnd, &r);
            g_ctrl->put_Bounds(r);
        }
        return 0;
    case WM_MOVE:
        if (g_ctrl)
            g_ctrl->NotifyParentWindowPositionChanged();
        return 0;
    case WM_ACTIVATE: // back from Notepad: CryMT.ini edited there is read again
        if (LOWORD(wp) != WA_INACTIVE && g_view && Exists(IniPath()) && !SameTime(WriteTime(IniPath()), g_iniTime))
            SendState(L"external");
        break;
    case WM_SETFOCUS:
        if (g_ctrl)
            g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_HOSTJSON:
    {
        auto* json = reinterpret_cast<std::wstring*>(lp);
        Send(*json);
        // After a start: minimise or close, as the page asked.
        if (json->find(L"\"type\":\"played\",\"ok\":true") != std::wstring::npos)
        {
            if (json->find(L"\"after\":\"close\"") != std::wstring::npos)
                PostMessageW(wnd, WM_CLOSE, 0, 0);
            else if (json->find(L"\"after\":\"minimize\"") != std::wstring::npos)
                ShowWindow(wnd, SW_MINIMIZE);
        }
        if (json->find(L"\"restart\":true") != std::wstring::npos)
            SetTimer(wnd, 1, 1500, nullptr); // the page shows "restarting" first
        delete json;
        return 0;
    }
    case WM_TIMER:
    {
        KillTimer(wnd, wp);
        Restart();
        return 0;
    }
    case WM_DESTROY:
        if (g_ctrl)
        {
            g_ctrl->Close();
            g_ctrl->Release();
            g_ctrl = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

std::wstring Arg(const wchar_t* cmd, const wchar_t* name)
{
    const wchar_t* at = wcsstr(cmd, name);
    if (!at)
        return {};
    at += wcslen(name);
    const bool quoted = *at == L'"';
    if (quoted)
        at++;
    const wchar_t* end = quoted ? wcschr(at, L'"') : wcschr(at, L' ');
    return end ? std::wstring(at, end) : std::wstring(at);
}
std::wstring FolderPath(std::wstring path)
{
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    return path;
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show)
{
    // One launcher at a time: a second start brings the first one forward.
    g_once = CreateMutexW(nullptr, TRUE, L"CryMTLauncherOnce");
    if (GetLastError() == ERROR_ALREADY_EXISTS && !wcsstr(cmd, L"--restarted"))
    {
        if (const HWND other = FindWindowW(kClass, nullptr))
        {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_dir = self;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    g_game = Arg(cmd, L"--game=");
    if (g_game.empty())
    {
        g_game = g_dir.substr(0, g_dir.size() - 1);
        g_game.resize(g_game.find_last_of(L'\\') + 1);
    }
    g_game = FolderPath(g_game);
    g_ui = Arg(cmd, L"--ui=");
    g_ui = FolderPath(g_ui.empty() ? g_dir + L"ui" : g_ui);
    g_dev = wcsstr(cmd, L"--dev") != nullptr;
    g_page = Arg(cmd, L"--page=");
    CreateDirectoryW((g_dir + L"cache").c_str(), nullptr);
    // Left by a self-update (the running launcher and its loader were renamed to .old); the old process may still be
    // ending, so a few tries.
    for (int i = 0; i < 30; i++)
    {
        const bool a = DeleteFileW((g_dir + L"CryMTLauncher.exe.old").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        const bool b = DeleteFileW((g_dir + L"WebView2Loader.dll.old").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        if (a && b)
            break;
        Sleep(100);
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBack);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // Centred on the monitor under the cursor, at its DPI.
    POINT cursor;
    GetCursorPos(&cursor);
    const HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    if (const HMODULE shcore = LoadLibraryW(L"shcore.dll"))
        if (const auto getDpi = reinterpret_cast<HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*)>(GetProcAddress(shcore, "GetDpiForMonitor")))
        {
            UINT y = 96;
            getDpi(mon, 0, &dpi, &y);
        }
    const int w = MulDiv(kWidth, dpi, 96), h = MulDiv(kHeight, dpi, 96);
    const RECT& work = mi.rcWork;
    // WS_CAPTION / WS_THICKFRAME keep the shadow, the taskbar's minimise and Aero snap; WM_NCCALCSIZE hides the frame.
    g_wnd = CreateWindowExW(0, kClass, L"CryMT", WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
                            work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    const MARGINS shadow{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(g_wnd, &shadow);
    const DWORD square = 1; // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND (Windows 11): the menu look has square corners
    DwmSetWindowAttribute(g_wnd, 33, &square, sizeof(square));
    SetWindowPos(g_wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g_wnd, show);
    CreateView();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
