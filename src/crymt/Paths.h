// Where CryMT keeps its files: next to CryMT.dll, so a package works from any folder. Logs in logs\mt, frame
// captures in logs\play, the pipeline recipes in cache, the NVIDIA Streamline runtime in streamline.
#pragma once
#include <string>

const std::wstring& CryMTDirW(); // the DLL's folder, with a trailing backslash
std::string CryMTLog(const char* name);   // logs\mt\<name> (the folder is created on first use)
std::string CryMTCapture(const char* name); // logs\play\<name>
std::wstring CryMTCacheW(const wchar_t* name); // cache\<name>
