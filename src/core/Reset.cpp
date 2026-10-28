// SPDX-License-Identifier: MIT
#include "Reset.h"

#include <winevt.h>

#include <algorithm>

#include "Targets.h"

namespace windira {

namespace {

// Services that only broker camera access and restart on demand. Restarting them makes
// every app drop its camera handles, which is exactly what a stuck camera needs.
const wchar_t* const kRestartableServices[] = {L"FrameServer", L"FrameServerMonitor"};

ULONGLONG FileTimeValue(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

std::wstring BaseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ProcessImagePath(DWORD pid, DWORD format) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(buf);
    std::wstring path;
    if (QueryFullProcessImageNameW(h, format, buf, &size)) path.assign(buf, size);
    CloseHandle(h);
    return path;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Translation {
        WORD language, codePage;
    }* tr = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < sizeof(Translation))
        return {};
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->language, tr->codePage);
    wchar_t* value = nullptr;
    if (!VerQueryValueW(data.data(), key, (void**)&value, &len) || !value) return {};
    return Trim(value);
}

std::vector<std::wstring> ServicesInProcess(DWORD pid) {
    std::vector<std::wstring> names;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return names;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, nullptr, 0, &needed, &count,
                          &resume, nullptr);
    std::vector<BYTE> buf(needed + 4096);
    resume = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buf.data(), (DWORD)buf.size(),
                              &needed, &count, &resume, nullptr)) {
        auto* items = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        for (DWORD i = 0; i < count; ++i)
            if (items[i].ServiceStatusProcess.dwProcessId == pid) names.emplace_back(items[i].lpServiceName);
    }
    CloseServiceHandle(scm);
    return names;
}

bool IsRestartableService(const std::wstring& name) {
    return std::any_of(std::begin(kRestartableServices), std::end(kRestartableServices),
                       [&](const wchar_t* s) { return IEquals(name, s); });
}

void Classify(Blocker& b) {
    // The event names the program by its NT path; if the pid now runs something else,
    // the original program has exited and its pid was reused.
    std::wstring native = ProcessImagePath(b.pid, PROCESS_NAME_NATIVE);
    std::wstring win32 = IEquals(native, b.imagePath) ? ProcessImagePath(b.pid, 0) : L"";
    if (!win32.empty()) b.services = ServicesInProcess(b.pid);
    if (!win32.empty()) {
        b.imagePath = win32;
        b.description = FileDescription(win32);
    }
    wchar_t windowsDir[MAX_PATH] = {};
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    bool windowsComponent = !win32.empty() && IContains(win32.substr(0, wcslen(windowsDir) + 1),
                                                        std::wstring(windowsDir) + L"\\");

    if (b.pid == GetCurrentProcessId()) {
        b.reason = L"this is Windira's own camera preview, which is stuck - close and reopen Windira";
    } else if (win32.empty()) {
        b.reason = L"the program has already exited";
    } else if (!b.services.empty()) {
        bool allRestartable = std::all_of(b.services.begin(), b.services.end(), IsRestartableService);
        if (allRestartable) b.canClose = true;  // restarted, not killed
        else b.reason = L"it is a Windows service";
    } else if (windowsComponent) {
        b.reason = L"it is part of Windows";
    } else {
        b.canClose = true;
    }
}

}  // namespace

std::vector<Blocker> FindBlockers(const std::wstring& deviceId, ULONGLONG since) {
    std::vector<Blocker> out;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = FileTimeValue(nowFt);
    ULONGLONG windowMs = (now > since ? (now - since) / 10000 : 0) + 60000;
    std::wstring xpath = L"*[System[Provider[@Name='Microsoft-Windows-Kernel-PnP'] and EventID=225 and "
                         L"TimeCreated[timediff(@SystemTime) <= " + std::to_wstring(windowMs) + L"]]]";
    EVT_HANDLE query = EvtQuery(nullptr, L"System", xpath.c_str(), EvtQueryChannelPath);
    if (!query) return out;
    LPCWSTR paths[] = {
        L"Event/System/TimeCreated/@SystemTime",
        L"Event/EventData/Data[@Name='ProcessId']",
        L"Event/EventData/Data[@Name='ProcessName']",
        L"Event/EventData/Data[@Name='DeviceInstance']",
        L"Event/EventData/Data[@Name='VetoingDevices']",
    };
    EVT_HANDLE context = EvtCreateRenderContext(ARRAYSIZE(paths), paths, EvtRenderContextValues);
    EVT_HANDLE events[16];
    DWORD returned = 0;
    std::vector<BYTE> buf(8192);
    while (context && EvtNext(query, ARRAYSIZE(events), events, 1000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            DWORD used = 0, count = 0;
            if (!EvtRender(context, events[i], EvtRenderEventValues, (DWORD)buf.size(), buf.data(), &used, &count) &&
                GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                buf.resize(used);
                EvtRender(context, events[i], EvtRenderEventValues, (DWORD)buf.size(), buf.data(), &used, &count);
            }
            EvtClose(events[i]);
            if (count < ARRAYSIZE(paths)) continue;
            auto* v = reinterpret_cast<EVT_VARIANT*>(buf.data());
            if (v[0].Type != EvtVarTypeFileTime || v[0].FileTimeVal < since) continue;
            auto str = [&](int idx) {
                return v[idx].Type == EvtVarTypeString && v[idx].StringVal ? std::wstring(v[idx].StringVal) : L"";
            };
            std::wstring device = str(3), vetoing = str(4);
            if (!IEquals(device, deviceId) && !IContains(vetoing, deviceId)) continue;
            Blocker b;
            b.pid = v[1].Type == EvtVarTypeUInt32 ? v[1].UInt32Val : 0;
            b.imagePath = str(2);
            b.fileName = BaseName(b.imagePath);
            if (!b.pid || std::any_of(out.begin(), out.end(), [&](const Blocker& o) { return o.pid == b.pid; }))
                continue;
            Classify(b);
            out.push_back(b);
        }
    }
    if (context) EvtClose(context);
    EvtClose(query);
    return out;
}

namespace {

Status RestartServices(const std::vector<std::wstring>& names) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return Status::Fail(L"Cannot open the service manager: " + Win32ErrorMessage(GetLastError()));
    Status result;
    std::vector<SC_HANDLE> stopped;
    for (const auto& name : names) {
        SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
        if (!svc) continue;
        SERVICE_STATUS status{};
        if (ControlService(svc, SERVICE_CONTROL_STOP, &status) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
            for (int i = 0; i < 50 && QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_STOPPED; ++i)
                Sleep(200);
            stopped.push_back(svc);
        } else {
            result = Status::Fail(L"Cannot stop " + name + L": " + Win32ErrorMessage(GetLastError()));
            CloseServiceHandle(svc);
        }
    }
    // Both services also start on demand; starting them now just saves the next app a moment.
    for (SC_HANDLE svc : stopped) {
        StartServiceW(svc, 0, nullptr);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return result;
}

}  // namespace

std::wstring Blocker::DisplayName() const {
    std::wstring name = fileName.empty() ? BaseName(imagePath) : fileName;
    if (!services.empty()) {
        std::wstring list;
        for (const auto& s : services) list += (list.empty() ? L"" : L", ") + s;
        return L"Windows service " + list + L" (" + name + L")";
    }
    if (!description.empty() && !IEquals(description, name)) return description + L" (" + name + L")";
    return name;
}

std::wstring ResetTargetFor(const Inventory& inv, const DevNodeInfo& dev) {
    if (IsNullOrRootContainer(dev.containerId)) return dev.instanceId;
    const DevNodeInfo* parent = inv.FindDevice(dev.parentId);
    if (parent && IEquals(parent->className, L"USB") && IEquals(parent->containerId, dev.containerId) &&
        !IContains(parent->instanceId, L"&MI_"))
        return parent->instanceId;
    return dev.instanceId;
}

ResetOutcome ResetDevice(const std::wstring& instanceId) {
    ResetOutcome out;
    out.deviceId = instanceId;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG since = FileTimeValue(now) - 10'000'000ULL;  // 1 s of clock slack

    // DIF_PROPERTYCHANGE is what Device Manager's disable/enable uses. If a program vetoes,
    // Windows leaves the device running untouched and asks for a reboot instead.
    bool rebootRequired = false;
    Status s = RestartDevice(instanceId, rebootRequired);
    if (!s) {
        out.message = s.message;
        return out;
    }
    if (!rebootRequired) {
        out.result = ResetOutcome::Result::Restarted;
        return out;
    }
    // The veto event is written by the kernel as the request fails; give it a moment.
    for (int i = 0; i < 6 && out.blockers.empty(); ++i) {
        out.blockers = FindBlockers(instanceId, since);
        if (out.blockers.empty()) Sleep(250);
    }
    out.result = out.blockers.empty() ? ResetOutcome::Result::RebootRequired : ResetOutcome::Result::Blocked;
    return out;
}

std::vector<std::wstring> CloseBlockers(const std::vector<Blocker>& blockers) {
    std::vector<std::wstring> log;
    for (const Blocker& b : blockers) {
        if (!b.canClose) {
            log.push_back(L"Left running: " + b.DisplayName() + L" - " + b.reason);
            continue;
        }
        if (!b.services.empty()) {
            Status s = RestartServices(b.services);
            log.push_back(s ? L"Restarted " + b.DisplayName() : s.message);
            continue;
        }
        HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, b.pid);
        if (!h) {
            log.push_back(L"Already closed: " + b.DisplayName());
            continue;
        }
        // Make sure the process id was not reused by another program since the event.
        wchar_t path[MAX_PATH * 2];
        DWORD size = ARRAYSIZE(path);
        bool same = QueryFullProcessImageNameW(h, 0, path, &size) && IEquals(std::wstring(path, size), b.imagePath);
        if (!same) {
            log.push_back(L"Already closed: " + b.DisplayName());
        } else if (TerminateProcess(h, 1)) {
            WaitForSingleObject(h, 5000);
            log.push_back(L"Closed " + b.DisplayName());
        } else {
            log.push_back(L"Could not close " + b.DisplayName() + L": " + Win32ErrorMessage(GetLastError()));
        }
        CloseHandle(h);
    }
    return log;
}

}  // namespace windira
