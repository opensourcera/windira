// SPDX-License-Identifier: MIT
// windira-cli - command line interface for Windira.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/Common.h"

#include <mmdeviceapi.h>

#include "../core/Devices.h"
#include "../core/Json.h"
#include "../core/Renamer.h"
#include "../core/Reset.h"
#include "../core/State.h"
#include "../core/Task.h"

using namespace windira;

namespace {

bool g_quiet = false;

void Write(HANDLE h, const std::wstring& text) {
    DWORD mode, written;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &written, nullptr);
    } else {
        std::string utf8 = WideToUtf8(text);
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    }
}

void Out(const std::wstring& text) {
    if (!g_quiet) Write(GetStdHandle(STD_OUTPUT_HANDLE), text + L"\n");
}

void Err(const std::wstring& text) {
    Write(GetStdHandle(STD_ERROR_HANDLE), text + L"\n");
}

std::wstring Pad(const std::wstring& s, size_t width) {
    if (s.size() >= width) return s + L"  ";
    return s + std::wstring(width - s.size(), L' ');
}

void AppendLog(const std::wstring& line) {
    if (!EnsureSecureDirectory(ProgramDataDirectory())) return;
    std::wstring path = ProgramDataDirectory() + L"\\reapply.log";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 512 * 1024) {
        CloseHandle(f);
        f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
    }
    std::string utf8 = WideToUtf8(NowIso8601() + L"  " + line + L"\r\n");
    DWORD written;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);
}

int Usage() {
    Out(std::wstring(L"Windira ") + kAppVersion + L" - rename devices on Windows 10 and 11\n"
        L"\n"
        L"Usage: windira-cli <command> [options]\n"
        L"\n"
        L"  list [--all] [--json]           Cameras, media and audio devices (--all: every device)\n"
        L"  show <device>                   Every name Windows stores for a device or endpoint\n"
        L"  rename <device> <name>          Rename a device everywhere Windows shows it\n"
        L"      --only-this                 Do not rename related parts (webcam microphone, ...)\n"
        L"      --restart                   Restart the device afterwards\n"
        L"      --what-if                   Show what would change, change nothing\n"
        L"  rename-endpoint <endpoint> [--description <text>] [--device <text>] [--what-if]\n"
        L"                                  Rename a sound endpoint: \"<description> (<device>)\"\n"
        L"  restore <device|endpoint>       Put back the original names\n"
        L"  restore --all                   Undo every rename Windira made\n"
        L"  reset <device> [--close-blockers]\n"
        L"                                  Restart a device like a replug; names any program\n"
        L"                                  holding it, and with --close-blockers closes it\n"
        L"  history                         List renames Windira keeps\n"
        L"  reapply [--quiet]               Re-apply names Windows has reset\n"
        L"  task install|remove|status      Re-apply automatically after driver updates\n"
        L"\n"
        L"<device> is an instance id (as printed by list), a unique part of one, or the current\n"
        L"name. Changing names requires an elevated (Run as administrator) terminal.");
    return 2;
}

struct Selection {
    const DevNodeInfo* dev = nullptr;
    const AudioEndpointInfo* ep = nullptr;
};

bool Resolve(const Inventory& inv, const std::wstring& sel, Selection& out) {
    out = {};
    if (const DevNodeInfo* d = inv.FindDevice(sel)) { out.dev = d; return true; }
    if (const AudioEndpointInfo* e = inv.FindEndpoint(sel)) { out.ep = e; return true; }

    std::vector<Selection> matches;
    auto collect = [&](auto devPred, auto epPred) {
        matches.clear();
        for (const auto& d : inv.devices)
            if (devPred(d)) matches.push_back({&d, nullptr});
        for (const auto& e : inv.endpoints)
            if (epPred(e)) matches.push_back({nullptr, &e});
    };
    // Endpoint device nodes (SWD\MMDEVAPI) mirror the endpoints; match names on the endpoint only.
    auto named = [](const DevNodeInfo& d) { return !IEquals(d.className, L"AudioEndpoint"); };
    collect([&](const DevNodeInfo& d) { return named(d) && IEquals(d.DisplayName(), sel); },
            [&](const AudioEndpointInfo& e) { return IEquals(e.friendlyName, sel); });
    if (matches.empty())
        collect([&](const DevNodeInfo& d) { return IContains(d.instanceId, sel); },
                [&](const AudioEndpointInfo& e) { return IContains(e.id, sel); });
    if (matches.empty())
        collect([&](const DevNodeInfo& d) { return named(d) && IsMediaClass(d.className) && IContains(d.DisplayName(), sel); },
                [&](const AudioEndpointInfo& e) { return IContains(e.friendlyName, sel); });

    // A device renamed with its related parts shares its name with them (camera, audio
    // function, USB entry). When every match is part of one physical device, pick the
    // most specific function: camera, then imaging, then sound.
    if (matches.size() > 1 && std::all_of(matches.begin(), matches.end(), [&](const Selection& m) {
            return m.dev && !IsNullOrRootContainer(m.dev->containerId) &&
                   IEquals(m.dev->containerId, matches[0].dev ? matches[0].dev->containerId : L"");
        })) {
        for (const wchar_t* cls : {L"Camera", L"Image", L"MEDIA"}) {
            auto it = std::find_if(matches.begin(), matches.end(),
                                   [&](const Selection& m) { return IEquals(m.dev->className, cls); });
            if (it != matches.end()) {
                out = *it;
                return true;
            }
        }
    }
    if (matches.size() == 1) {
        out = matches[0];
        return true;
    }
    if (matches.empty()) {
        Err(L"No device matches \"" + sel + L"\". Run \"windira-cli list\" to see device ids.");
    } else {
        Err(L"\"" + sel + L"\" matches " + std::to_wstring(matches.size()) + L" devices; use the full id:");
        for (const auto& m : matches)
            Err(m.dev ? L"  " + m.dev->DisplayName() + L"  " + m.dev->instanceId
                      : L"  " + m.ep->friendlyName + L"  " + m.ep->id);
    }
    return false;
}

bool RequireAdmin() {
    if (IsElevated()) return true;
    Err(L"This needs administrator rights. Open the terminal with \"Run as administrator\".");
    return false;
}

int PrintResults(const std::vector<ChangeResult>& results, bool showSkipped = true) {
    int failures = 0;
    for (const auto& r : results) {
        if (r.skipped && !showSkipped) continue;
        std::wstring mark = !r.status ? L"  FAILED  " : r.skipped ? L"  skipped " : L"  ok      ";
        std::wstring line = mark + r.change.label;
        if (!r.note.empty()) line += L"  (" + r.note + L")";
        if (!r.status) {
            ++failures;
            line += L"\n            " + r.status.message;
            Err(line);
        } else {
            Out(line);
        }
    }
    return failures;
}

json::Value DeviceJson(const DevNodeInfo& d, const StateStore& state) {
    json::Value o = json::Value::MakeObject();
    o.Set("type", "device");
    o.Set("id", WideToUtf8(d.instanceId));
    o.Set("name", WideToUtf8(d.DisplayName()));
    o.Set("description", WideToUtf8(d.deviceDesc));
    o.Set("class", WideToUtf8(d.className));
    o.Set("manufacturer", WideToUtf8(d.manufacturer));
    o.Set("location", WideToUtf8(d.locationInfo));
    o.Set("locationPath", WideToUtf8(d.locationPath));
    o.Set("serial", WideToUtf8(d.serial));
    o.Set("containerId", WideToUtf8(d.containerId));
    o.Set("renamedByWindira", state.IsTracked(d.instanceId));
    json::Value itfs = json::Value::MakeArray();
    for (const auto& i : d.interfaces) {
        if (!i.hasFriendlyName) continue;
        json::Value io = json::Value::MakeObject();
        io.Set("category", WideToUtf8(InterfaceCategoryName(i.classGuid)));
        io.Set("name", WideToUtf8(i.friendlyName));
        io.Set("link", WideToUtf8(i.symbolicLink));
        itfs.Push(std::move(io));
    }
    o.Set("interfaces", std::move(itfs));
    return o;
}

json::Value EndpointJson(const AudioEndpointInfo& e, const StateStore& state) {
    json::Value o = json::Value::MakeObject();
    o.Set("type", e.capture ? "recording" : "playback");
    o.Set("id", WideToUtf8(e.id));
    o.Set("name", WideToUtf8(e.friendlyName));
    o.Set("description", WideToUtf8(e.deviceDesc));
    o.Set("device", WideToUtf8(e.interfaceName));
    o.Set("containerId", WideToUtf8(e.containerId));
    o.Set("enabled", e.state == DEVICE_STATE_ACTIVE);
    o.Set("renamedByWindira", state.IsTracked(e.id));
    return o;
}

int CmdList(const Inventory& inv, const StateStore& state, bool all, bool asJson) {
    if (asJson) {
        json::Value arr = json::Value::MakeArray();
        for (const auto& d : inv.devices)
            if (all || (IsMediaClass(d.className) && !IEquals(d.className, L"AudioEndpoint")))
                arr.Push(DeviceJson(d, state));
        for (const auto& e : inv.endpoints) arr.Push(EndpointJson(e, state));
        Write(GetStdHandle(STD_OUTPUT_HANDLE), Utf8ToWide(arr.Serialize(2)) + L"\n");
        return 0;
    }
    std::wstring lastClass;
    for (const auto& d : inv.devices) {
        if (!all && (!IsMediaClass(d.className) || IEquals(d.className, L"AudioEndpoint"))) continue;
        if (!IEquals(d.className, lastClass)) {
            Out(L"\n[" + (d.className.empty() ? std::wstring(L"(no class)") : d.className) + L"]");
            lastClass = d.className;
        }
        std::wstring line = L"  " + Pad(d.DisplayName(), 40);
        if (!d.locationInfo.empty()) line += Pad(d.locationInfo, 24);
        if (!d.serial.empty()) line += L"serial " + d.serial + L"  ";
        if (state.IsTracked(d.instanceId)) line += L"* renamed";
        Out(line);
        Out(L"      " + d.instanceId);
    }
    for (int pass = 0; pass < 2; ++pass) {
        bool capture = pass == 0;
        bool header = false;
        for (const auto& e : inv.endpoints) {
            if (e.capture != capture) continue;
            if (!header) {
                Out(capture ? L"\n[Sound: recording]" : L"\n[Sound: playback]");
                header = true;
            }
            std::wstring line = L"  " + Pad(e.friendlyName, 40);
            if (e.state != DEVICE_STATE_ACTIVE) line += L"disabled  ";
            if (state.IsTracked(e.id)) line += L"* renamed";
            Out(line);
            Out(L"      " + e.id);
        }
    }
    Out(L"");
    return 0;
}

int CmdShow(const Inventory& inv, const StateStore& state, const std::wstring& sel) {
    Selection s;
    if (!Resolve(inv, sel, s)) return 1;
    if (s.ep) {
        const auto& e = *s.ep;
        Out(e.friendlyName);
        Out(L"  Id:            " + e.id);
        Out(L"  Kind:          " + std::wstring(e.capture ? L"Recording" : L"Playback"));
        Out(L"  Description:   " + e.deviceDesc);
        Out(L"  Device name:   " + e.interfaceName);
        Out(L"  Container:     " + e.containerId);
    } else {
        const auto& d = *s.dev;
        Out(d.DisplayName());
        Out(L"  Instance id:   " + d.instanceId);
        Out(L"  Class:         " + d.className);
        Out(L"  Friendly name: " + (d.hasFriendlyName ? d.friendlyName : std::wstring(L"(not set)")));
        Out(L"  Description:   " + d.deviceDesc);
        Out(L"  Manufacturer:  " + d.manufacturer);
        Out(L"  Location:      " + d.locationInfo);
        Out(L"  Location path: " + d.locationPath);
        Out(L"  Serial:        " + (d.serial.empty() ? std::wstring(L"(none - identified by USB port)") : d.serial));
        Out(L"  Container:     " + d.containerId);
        Out(L"  Parent:        " + d.parentId);
        Out(L"  Interfaces with a name (what camera/audio apps show):");
        size_t n = 0;
        for (const auto& i : d.interfaces) {
            if (!i.hasFriendlyName) continue;
            ++n;
            Out(L"    " + Pad(InterfaceCategoryName(i.classGuid), 16) + i.friendlyName);
            Out(L"      " + i.symbolicLink);
        }
        if (!n) Out(L"    (none)");
        if (!IsNullOrRootContainer(d.containerId)) {
            Out(L"  Same physical device:");
            for (const auto& o : inv.devices)
                if (&o != &d && IEquals(o.containerId, d.containerId))
                    Out(L"    " + Pad(o.className, 16) + o.DisplayName());
            for (const auto& e : inv.endpoints)
                if (IEquals(e.containerId, d.containerId)) Out(L"    " + Pad(L"Sound", 16) + e.friendlyName);
        }
    }
    std::wstring key = s.dev ? s.dev->instanceId : s.ep->id;
    if (state.IsTracked(key)) Out(L"  Renamed by Windira - \"windira-cli restore\" puts the original names back.");
    return 0;
}

int CmdRename(const Inventory& inv, StateStore& state, const std::vector<std::wstring>& args) {
    std::vector<std::wstring> pos;
    bool onlyThis = false, restart = false, whatIf = false;
    for (const auto& a : args) {
        if (a == L"--only-this") onlyThis = true;
        else if (a == L"--restart") restart = true;
        else if (a == L"--what-if") whatIf = true;
        else pos.push_back(a);
    }
    if (pos.size() != 2) return Usage();
    std::wstring name = Trim(pos[1]);
    if (name.empty()) {
        Err(L"The new name cannot be empty.");
        return 2;
    }
    Selection s;
    if (!Resolve(inv, pos[0], s)) return 1;

    std::vector<Change> plan;
    if (s.dev) {
        plan = PlanDeviceRename(inv, *s.dev, name, !onlyThis);
        Out(L"Renaming \"" + s.dev->DisplayName() + L"\" to \"" + name + L"\":");
    } else {
        plan = PlanEndpointRename(*s.ep, nullptr, &name);
        Out(L"Renaming the device part of \"" + s.ep->friendlyName + L"\" to \"" + name + L"\":");
    }
    if (whatIf) {
        for (const auto& c : plan) Out(L"  would change  " + c.label);
        return 0;
    }
    if (!RequireAdmin()) return 1;
    auto results = ApplyChanges(state, plan);
    int failures = PrintResults(results);
    if (restart) {
        for (const auto& id : DevicesInChanges(results)) {
            bool reboot = false;
            Status rs = RestartDevice(id, reboot);
            if (!rs) Err(L"  restart failed: " + rs.message);
            else if (reboot) Out(L"  Windows needs a reboot to restart " + id);
        }
    }
    if (!failures) Out(L"Done. Apps that are already open may need to be restarted to show the new name.");
    return failures ? 1 : 0;
}

int CmdRenameEndpoint(const Inventory& inv, StateStore& state, const std::vector<std::wstring>& args) {
    std::wstring sel, desc, dev;
    bool hasDesc = false, hasDev = false, whatIf = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--description" && i + 1 < args.size()) { desc = Trim(args[++i]); hasDesc = true; }
        else if (args[i] == L"--device" && i + 1 < args.size()) { dev = Trim(args[++i]); hasDev = true; }
        else if (args[i] == L"--what-if") whatIf = true;
        else if (sel.empty()) sel = args[i];
        else return Usage();
    }
    if (sel.empty() || (!hasDesc && !hasDev)) return Usage();
    if ((hasDesc && desc.empty()) || (hasDev && dev.empty())) {
        Err(L"Names cannot be empty.");
        return 2;
    }
    Selection s;
    if (!Resolve(inv, sel, s)) return 1;
    if (!s.ep) {
        Err(L"\"" + sel + L"\" is not a sound endpoint. Use \"rename\" for devices.");
        return 1;
    }
    auto plan = PlanEndpointRename(*s.ep, hasDesc ? &desc : nullptr, hasDev ? &dev : nullptr);
    std::wstring result = (hasDesc ? desc : s.ep->deviceDesc) + L" (" + (hasDev ? dev : s.ep->interfaceName) + L")";
    Out(L"Renaming \"" + s.ep->friendlyName + L"\" to \"" + result + L"\":");
    if (whatIf) {
        for (const auto& c : plan) Out(L"  would change  " + c.label);
        return 0;
    }
    if (!RequireAdmin()) return 1;
    return PrintResults(ApplyChanges(state, plan)) ? 1 : 0;
}

int CmdRestore(const Inventory& inv, StateStore& state, const std::vector<std::wstring>& args) {
    if (args.size() != 1) return Usage();
    if (!RequireAdmin()) return 1;
    std::vector<ChangeResult> results;
    if (args[0] == L"--all") {
        results = RestoreAll(state);
    } else {
        std::wstring key = args[0];
        // Allow restoring devices that are not connected right now by their exact id.
        if (!state.IsTracked(key)) {
            Selection s;
            if (!Resolve(inv, args[0], s)) return 1;
            key = s.dev ? s.dev->instanceId : s.ep->id;
        }
        results = Restore(state, key);
    }
    if (results.empty()) {
        Out(L"Nothing to restore - Windira has not renamed this.");
        return 0;
    }
    return PrintResults(results) ? 1 : 0;
}

int CmdHistory(const StateStore& state) {
    if (state.entries().empty()) {
        Out(L"Windira has not renamed anything yet.");
        return 0;
    }
    Out(L"Renames kept in " + state.path() + L":");
    std::wstring lastGroup;
    for (const auto& e : state.entries()) {
        if (!IEquals(e.group, lastGroup)) {
            Out(L"\n" + e.group);
            lastGroup = e.group;
        }
        Out(L"  " + Pad(TargetKindLabel(e.kind), 30) + L"\"" + e.desired + L"\"  (was " +
            (e.original.exists ? L"\"" + e.original.value + L"\"" : std::wstring(L"not set")) + L")");
    }
    return 0;
}

int CmdReapply(const Inventory& inv, StateStore& state) {
    if (!RequireAdmin()) return 1;
    auto results = Reapply(state, inv);
    int failures = 0, applied = 0;
    for (const auto& r : results) {
        if (!r.status) {
            ++failures;
            AppendLog(L"FAILED " + r.change.label + L": " + r.status.message);
        } else if (!r.skipped) {
            ++applied;
            AppendLog(L"re-applied \"" + r.change.value.value + L"\" to " + r.change.label);
        }
    }
    PrintResults(results, false);
    Out(std::to_wstring(applied) + L" name(s) re-applied, " + std::to_wstring(failures) + L" failed, " +
        std::to_wstring(results.size() - applied - failures) + L" unchanged or not connected.");
    return failures ? 1 : 0;
}

void PrintBlockers(const ResetOutcome& r) {
    for (const auto& b : r.blockers) {
        std::wstring line = L"  " + b.DisplayName() + L"  (pid " + std::to_wstring(b.pid) + L")";
        if (!b.canClose) line += L"  - cannot be closed: " + b.reason;
        Out(line);
    }
}

int CmdReset(const Inventory& inv, const std::vector<std::wstring>& args) {
    std::wstring sel;
    bool closeBlockers = false;
    for (const auto& a : args) {
        if (a == L"--close-blockers") closeBlockers = true;
        else if (sel.empty()) sel = a;
        else return Usage();
    }
    if (sel.empty()) return Usage();
    Selection s;
    if (!Resolve(inv, sel, s)) return 1;
    if (!s.dev) {
        Err(L"Sound endpoints cannot be reset directly; reset the device they belong to.");
        return 1;
    }
    if (!RequireAdmin()) return 1;

    std::wstring target = ResetTargetFor(inv, *s.dev);
    const DevNodeInfo* t = inv.FindDevice(target);
    Out(L"Resetting " + (t ? t->DisplayName() : target) + L" (" + target + L")...");
    for (int attempt = 0;; ++attempt) {
        ResetOutcome r = ResetDevice(target);
        switch (r.result) {
            case ResetOutcome::Result::Restarted:
                Out(L"Done - the device restarted.");
                return 0;
            case ResetOutcome::Result::Failed:
                Err(r.message);
                return 1;
            case ResetOutcome::Result::RebootRequired:
                Out(L"Windows could not restart the device and did not report which program holds it.");
                Out(L"Unplug it and plug it back in, or restart Windows.");
                return 1;
            case ResetOutcome::Result::Blocked:
                Out(L"These programs refused to release the device:");
                PrintBlockers(r);
                bool anyClosable = std::any_of(r.blockers.begin(), r.blockers.end(),
                                               [](const Blocker& b) { return b.canClose; });
                if (!closeBlockers || attempt > 0 || !anyClosable) {
                    if (attempt > 0) Out(L"The device is still held. Restart Windows to reset it.");
                    else if (anyClosable) Out(L"Run again with --close-blockers to close them and retry.");
                    else Out(L"Restart Windows to reset it.");
                    return 1;
                }
                for (const auto& line : CloseBlockers(r.blockers)) Out(L"  " + line);
                Sleep(500);
                Out(L"Retrying...");
                break;
        }
    }
}

int CmdTask(const std::vector<std::wstring>& args) {
    if (args.size() != 1) return Usage();
    if (args[0] == L"status") {
        Out(IsReapplyTaskInstalled() ? L"Automatic re-apply is ON." : L"Automatic re-apply is OFF.");
        return 0;
    }
    if (!RequireAdmin()) return 1;
    Status s;
    if (args[0] == L"install") {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        s = InstallReapplyTask(self);
        if (s) Out(L"Automatic re-apply is ON. Names are checked at startup and after every driver install.");
    } else if (args[0] == L"remove") {
        s = RemoveReapplyTask();
        if (s) Out(L"Automatic re-apply is OFF.");
    } else {
        return Usage();
    }
    if (!s) Err(s.message);
    return s ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == L"help" || args[0] == L"--help" || args[0] == L"-h" || args[0] == L"/?") return Usage();
    std::wstring cmd = args[0];
    args.erase(args.begin());
    for (auto it = args.begin(); it != args.end();) {
        if (*it == L"--quiet") {
            g_quiet = true;
            it = args.erase(it);
        } else {
            ++it;
        }
    }

    if (cmd == L"version" || cmd == L"--version") {
        Out(std::wstring(L"Windira ") + kAppVersion);
        return 0;
    }
    if (cmd == L"task") return CmdTask(args);

    ComInit com(COINIT_MULTITHREADED);
    StateStore state;
    Status st = state.Load();
    if (!st) {
        Err(st.message);
        return 1;
    }
    if (cmd == L"history") return CmdHistory(state);

    Inventory inv;
    st = inv.Load();
    if (!st) {
        Err(st.message);
        return 1;
    }

    if (cmd == L"list") {
        bool all = false, asJson = false;
        for (const auto& a : args) {
            if (a == L"--all") all = true;
            else if (a == L"--json") asJson = true;
            else return Usage();
        }
        return CmdList(inv, state, all, asJson);
    }
    if (cmd == L"show") return args.size() == 1 ? CmdShow(inv, state, args[0]) : Usage();
    if (cmd == L"rename") return CmdRename(inv, state, args);
    if (cmd == L"rename-endpoint") return CmdRenameEndpoint(inv, state, args);
    if (cmd == L"restore") return CmdRestore(inv, state, args);
    if (cmd == L"reapply") return CmdReapply(inv, state);
    if (cmd == L"reset") return CmdReset(inv, args);
    return Usage();
}
