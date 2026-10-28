// Windira - device renamer for Windows
// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

namespace windira {

constexpr const wchar_t* kAppName = L"Windira";
constexpr const wchar_t* kAppVersion = L"1.0b";

// Lightweight success/failure value with a human readable message.
struct Status {
    bool ok = true;
    std::wstring message;

    static Status Ok() { return {}; }
    static Status Fail(std::wstring msg) {
        Status s;
        s.ok = false;
        s.message = std::move(msg);
        return s;
    }
    explicit operator bool() const { return ok; }
};

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

std::wstring ToLower(std::wstring s);
bool IEquals(const std::wstring& a, const std::wstring& b);
bool IContains(const std::wstring& haystack, const std::wstring& needle);
std::wstring Trim(const std::wstring& s);

std::wstring GuidToString(const GUID& g);
bool StringToGuid(const std::wstring& s, GUID& out);
bool IsNullOrRootContainer(const std::wstring& containerId);

std::wstring Win32ErrorMessage(DWORD code);
std::wstring HResultMessage(HRESULT hr);

bool IsElevated();
std::wstring ExecutableDirectory();
std::wstring ProgramDataDirectory();   // %ProgramData%\Windira - rename history and log
std::wstring ProgramFilesDirectory();  // %ProgramFiles%\Windira - where the re-apply task runs from
// Creates the directory (or takes over an existing one) writable only by admins and SYSTEM.
Status EnsureSecureDirectory(const std::wstring& dir);

// Scoped COM initialisation that tolerates an already-initialised thread.
class ComInit {
public:
    explicit ComInit(DWORD model = COINIT_MULTITHREADED);
    ~ComInit();
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    bool uninit_ = false;
};

}  // namespace windira
