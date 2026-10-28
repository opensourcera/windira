// SPDX-License-Identifier: MIT
#include "Common.h"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>

namespace windira {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool IContains(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring GuidToString(const GUID& g) {
    wchar_t buf[64] = {};
    StringFromGUID2(g, buf, 64);
    return buf;
}

bool StringToGuid(const std::wstring& s, GUID& out) {
    std::wstring t = s;
    if (!t.empty() && t.front() != L'{') t = L"{" + t + L"}";
    return SUCCEEDED(CLSIDFromString(t.c_str(), &out));
}

bool IsNullOrRootContainer(const std::wstring& containerId) {
    // {00000000-0000-0000-FFFF-FFFFFFFFFFFF} is the "this computer" container that
    // every built-in device belongs to, so it must never be used for grouping.
    return containerId.empty() || IEquals(containerId, L"{00000000-0000-0000-0000-000000000000}") ||
           IEquals(containerId, L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}");
}

std::wstring Win32ErrorMessage(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring msg = n ? Trim(std::wstring(buf, n)) : L"Unknown error";
    if (buf) LocalFree(buf);
    wchar_t code_s[32];
    swprintf_s(code_s, L" (0x%08lX)", code);
    return msg + code_s;
}

std::wstring HResultMessage(HRESULT hr) {
    return Win32ErrorMessage((DWORD)hr);
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t* fallback) {
    PWSTR base = nullptr;
    std::wstring dir = fallback;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base))) dir = base;
    if (base) CoTaskMemFree(base);
    return dir;
}

std::wstring ProgramDataDirectory() {
    return KnownFolder(FOLDERID_ProgramData, L"C:\\ProgramData") + L"\\Windira";
}

std::wstring ProgramFilesDirectory() {
    return KnownFolder(FOLDERID_ProgramFiles, L"C:\\Program Files") + L"\\Windira";
}

Status EnsureSecureDirectory(const std::wstring& dir) {
    // The re-apply task runs as SYSTEM and trusts these files, so only administrators
    // and SYSTEM may write them: owner Administrators, protected DACL, users read-only.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)", SDDL_REVISION_1, &sd, nullptr))
        return Status::Fail(L"Cannot build security descriptor: " + Win32ErrorMessage(GetLastError()));
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    Status result;
    if (!CreateDirectoryW(dir.c_str(), &sa)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            result = Status::Fail(L"Cannot create " + dir + L": " + Win32ErrorMessage(err));
        } else {
            // Someone created it before us: take it over and reset its permissions.
            BOOL present = FALSE, defaulted = FALSE;
            PACL dacl = nullptr;
            PSID owner = nullptr;
            GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
            GetSecurityDescriptorOwner(sd, &owner, &defaulted);
            DWORD rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
                                             OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                                 PROTECTED_DACL_SECURITY_INFORMATION,
                                             owner, nullptr, dacl, nullptr);
            if (rc != ERROR_SUCCESS)
                result = Status::Fail(L"Cannot secure " + dir + L": " + Win32ErrorMessage(rc));
        }
    }
    LocalFree(sd);
    return result;
}

ComInit::ComInit(DWORD model) {
    HRESULT hr = CoInitializeEx(nullptr, model);
    uninit_ = SUCCEEDED(hr);  // S_OK or S_FALSE; RPC_E_CHANGED_MODE means someone else owns it
}

ComInit::~ComInit() {
    if (uninit_) CoUninitialize();
}

}  // namespace windira
