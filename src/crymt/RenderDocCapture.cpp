// RenderDoc (https://renderdoc.org, MIT; the header in sdk\renderdoc) inside CryMT, for looking at single frames:
// every draw with its state, textures and buffers at each step, pixel history.
// - CRYMT_RENDERDOC=1 loads <CryMT>\renderdoc\renderdoc.dll, =<folder or dll path> another one. It has to be in the
//   process before the D3D12 device and the DXGI factory are created (InitCore calls Load first), so it sees them.
//   A game started from RenderDoc's own UI already has it; then only the API is taken.
// - PgUp captures the next frame into <CryMT>\logs\renderdoc\crysis_frame<N>.rdc (RenderDoc's own F12 / PrtScn and
//   F11 are off: F-keys are the game's debug keys). The overlay shows only the list of captures.
// - DLSS stays off while RenderDoc is in the process (RenderDoc does not capture NGX's work), so the capture shows
//   the frame without DLAA and without the camera jitter.
// - The passes carry names (render list, filter, recursion level; CryMT's own passes) for RenderDoc's event browser.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "renderdoc_app.h"
#include "Paths.h"
#include "RenderDocCapture.h"

namespace rdoc
{
namespace
{
RENDERDOC_API_1_6_0* g_api;
bool g_tried;
bool g_keyDown;

void Note(const char* format, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, CryMTLog("renderdoc.txt").c_str(), "a") != 0 || !f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

std::string Utf8(const std::wstring& w)
{
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(s.empty() ? 0 : s.size() - 1);
    return s;
}
} // namespace

bool Requested()
{
    wchar_t value[8] = {};
    return GetEnvironmentVariableW(L"CRYMT_RENDERDOC", value, 8) != 0 && wcscmp(value, L"0") != 0;
}

bool Load()
{
    if (g_tried)
        return g_api != nullptr;
    g_tried = true;
    wchar_t value[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"CRYMT_RENDERDOC", value, MAX_PATH) || wcscmp(value, L"0") == 0)
        return false;
    std::wstring path = L"(already in the process)";
    HMODULE module = GetModuleHandleW(L"renderdoc.dll");
    if (!module)
    {
        path = wcscmp(value, L"1") == 0 ? CryMTDirW() + L"renderdoc" : std::wstring(value);
        if (path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".dll") != 0)
            path += L"\\renderdoc.dll";
        module = LoadLibraryW(path.c_str());
    }
    if (!module)
    {
        Note("renderdoc.dll not loaded from %s (error %lu)", Utf8(path).c_str(), GetLastError());
        return false;
    }
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    void* api = nullptr;
    if (!getApi || getApi(eRENDERDOC_API_Version_1_6_0, &api) != 1 || !api)
    {
        Note("renderdoc.dll from %s has no API 1.6", Utf8(path).c_str());
        return false;
    }
    g_api = static_cast<RENDERDOC_API_1_6_0*>(api);
    g_api->SetCaptureKeys(nullptr, 0);
    g_api->SetFocusToggleKeys(nullptr, 0);
    g_api->MaskOverlayBits(eRENDERDOC_Overlay_Enabled | eRENDERDOC_Overlay_CaptureList, 0);
    const std::wstring dir = CryMTDirW() + L"logs\\renderdoc";
    CreateDirectoryW((CryMTDirW() + L"logs").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    g_api->SetCaptureFilePathTemplate(Utf8(dir + L"\\crysis").c_str());
    int major = 0, minor = 0, patch = 0;
    g_api->GetAPIVersion(&major, &minor, &patch);
    Note("RenderDoc API %d.%d.%d from %s; PgUp captures into %s; DLSS off while it is loaded", major, minor, patch, Utf8(path).c_str(),
         Utf8(dir).c_str());
    return true;
}

bool Loaded()
{
    return g_api != nullptr;
}

void Trigger()
{
    if (!g_api)
        return;
    g_api->TriggerCapture();
    Note("capture requested (%u saved so far)", g_api->GetNumCaptures());
}

void Poll(bool focus)
{
    if (!g_api)
        return;
    const bool down = focus && (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    if (down && !g_keyDown)
        Trigger();
    g_keyDown = down;
}
} // namespace rdoc
