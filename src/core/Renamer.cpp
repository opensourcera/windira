// SPDX-License-Identifier: MIT
#include "Renamer.h"

#include <algorithm>

namespace windira {

namespace {

struct Category {
    const wchar_t* guid;
    const wchar_t* name;
};

const Category kCategories[] = {
    {L"{E5323777-F976-4F5B-9B55-B94699C46E44}", L"Video camera"},
    {L"{24E552D7-6523-47F7-A647-D3465BF1F5CA}", L"Sensor camera"},
    {L"{65E8773D-8F56-11D0-A3B9-00A0C9223196}", L"Capture"},
    {L"{65E8773E-8F56-11D0-A3B9-00A0C9223196}", L"Render"},
    {L"{6994AD05-93EF-11D0-A3CC-00A0C9223196}", L"Video"},
    {L"{6994AD04-93EF-11D0-A3CC-00A0C9223196}", L"Audio"},
    {L"{DDA54A40-1E4C-11D1-A050-405705C10000}", L"Topology"},
    {L"{A5DCBF10-6530-11D2-901F-00C04FB951ED}", L"USB device"},
};

void AddDevice(std::vector<Change>& out, const DevNodeInfo& dev, const std::wstring& name, const std::wstring& group) {
    Change c;
    c.kind = TargetKind::DeviceName;
    c.key = dev.instanceId;
    c.group = group;
    c.label = L"Device Manager: " + dev.DisplayName() + L" [" + dev.className + L"]";
    c.value = {true, name};
    out.push_back(c);
    for (const auto& itf : dev.interfaces) {
        if (!itf.hasFriendlyName) continue;
        Change ic;
        ic.kind = TargetKind::InterfaceName;
        ic.key = itf.symbolicLink;
        ic.group = group;
        ic.label = L"Apps: " + itf.friendlyName + L" [" + InterfaceCategoryName(itf.classGuid) + L" interface]";
        ic.value = {true, name};
        out.push_back(ic);
    }
}

ChangeResult WriteChange(const Change& c) {
    ChangeResult r;
    r.change = c;
    r.status = WriteTarget(c.kind, c.key, c.value);
    return r;
}

std::vector<RenameEntry*> Matching(StateStore& state, const std::wstring& groupOrKey) {
    std::vector<RenameEntry*> out;
    for (auto& e : state.entries())
        if (groupOrKey.empty() || IEquals(e.group, groupOrKey) || IEquals(e.key, groupOrKey)) out.push_back(&e);
    return out;
}

void SaveInto(StateStore& state, std::vector<ChangeResult>& results) {
    Status s = state.Save();
    if (!s) {
        ChangeResult r;
        r.change.label = L"Save rename history (" + state.path() + L")";
        r.status = s;
        results.push_back(r);
    }
}

std::vector<ChangeResult> RestoreMatching(StateStore& state, const std::wstring& groupOrKey) {
    std::vector<ChangeResult> results;
    std::vector<std::pair<TargetKind, std::wstring>> done;
    for (RenameEntry* e : Matching(state, groupOrKey)) {
        Change c;
        c.kind = e->kind;
        c.key = e->key;
        c.group = e->group;
        c.label = e->label;
        c.value = e->original;
        ChangeResult r = WriteChange(c);
        if (r.status) done.emplace_back(e->kind, e->key);
        results.push_back(r);
    }
    for (const auto& d : done) state.Remove(d.first, d.second);
    if (!done.empty()) SaveInto(state, results);
    return results;
}

}  // namespace

std::wstring InterfaceCategoryName(const std::wstring& classGuid) {
    for (const auto& c : kCategories)
        if (IEquals(classGuid, c.guid)) return c.name;
    return classGuid;
}

std::vector<Change> PlanDeviceRename(const Inventory& inv, const DevNodeInfo& dev, const std::wstring& name,
                                     bool includeRelated) {
    std::vector<Change> out;
    const std::wstring& group = dev.instanceId;
    AddDevice(out, dev, name, group);
    if (!includeRelated || IsNullOrRootContainer(dev.containerId)) return out;

    for (const auto& other : inv.devices) {
        if (&other == &dev || IEquals(other.instanceId, dev.instanceId)) continue;
        if (!IEquals(other.containerId, dev.containerId)) continue;
        // Audio endpoint device nodes (SWD\MMDEVAPI) are named by the audio service;
        // they are handled through the endpoint properties below.
        bool mediaFunction = IsMediaClass(other.className) && !IEquals(other.className, L"AudioEndpoint");
        bool compositeParent = IEquals(other.instanceId, dev.parentId) && IEquals(other.className, L"USB");
        if (mediaFunction || compositeParent) AddDevice(out, other, name, group);
    }
    for (const auto& ep : inv.endpoints) {
        if (!IEquals(ep.containerId, dev.containerId)) continue;
        Change c;
        c.kind = TargetKind::EndpointDeviceName;
        c.key = ep.id;
        c.group = group;
        c.label = L"Sound: " + ep.friendlyName;
        c.value = {true, name};
        out.push_back(c);
    }
    return out;
}

std::vector<Change> PlanEndpointRename(const AudioEndpointInfo& ep, const std::wstring* description,
                                       const std::wstring* deviceName) {
    std::vector<Change> out;
    if (description) {
        Change c;
        c.kind = TargetKind::EndpointDescription;
        c.key = ep.id;
        c.group = ep.id;
        c.label = L"Sound: " + ep.friendlyName + L" (description)";
        c.value = {true, *description};
        out.push_back(c);
    }
    if (deviceName) {
        Change c;
        c.kind = TargetKind::EndpointDeviceName;
        c.key = ep.id;
        c.group = ep.id;
        c.label = L"Sound: " + ep.friendlyName + L" (device name)";
        c.value = {true, *deviceName};
        out.push_back(c);
    }
    return out;
}

std::vector<ChangeResult> ApplyChanges(StateStore& state, const std::vector<Change>& changes) {
    std::vector<ChangeResult> results;
    bool dirty = false;
    for (const Change& c : changes) {
        ChangeResult r;
        r.change = c;
        TargetValue current;
        r.status = ReadTarget(c.kind, c.key, current);
        if (!r.status) {
            results.push_back(r);
            continue;
        }
        RenameEntry* existing = state.Find(c.kind, c.key);
        r.status = WriteTarget(c.kind, c.key, c.value);
        if (r.status) {
            if (!existing) {
                RenameEntry e;
                e.kind = c.kind;
                e.key = c.key;
                e.original = current;  // only the very first rename records the original
                state.entries().push_back(e);
                existing = &state.entries().back();
            }
            existing->group = c.group;
            existing->label = c.label;
            existing->desired = c.value.value;
            existing->updated = NowIso8601();
            dirty = true;
        }
        results.push_back(r);
    }
    if (dirty) SaveInto(state, results);
    return results;
}

std::vector<ChangeResult> Restore(StateStore& state, const std::wstring& groupOrKey) {
    if (groupOrKey.empty()) return {};
    return RestoreMatching(state, groupOrKey);
}

std::vector<ChangeResult> RestoreAll(StateStore& state) {
    return RestoreMatching(state, L"");
}

std::vector<ChangeResult> Reapply(StateStore& state, const Inventory& inv) {
    std::vector<ChangeResult> results;
    std::vector<Change> additions;

    for (const RenameEntry& e : state.entries()) {
        ChangeResult r;
        r.change.kind = e.kind;
        r.change.key = e.key;
        r.change.group = e.group;
        r.change.label = e.label;
        r.change.value = {true, e.desired};

        TargetValue current;
        if (!ReadTarget(e.kind, e.key, current)) {
            r.skipped = true;
            r.note = L"not connected";
        } else if (current.exists && current.value == e.desired) {
            r.skipped = true;
            r.note = L"already correct";
        } else {
            r.status = WriteTarget(e.kind, e.key, r.change.value);
            r.note = L"re-applied";
        }
        results.push_back(r);

        // A driver update can add interfaces the original rename never saw.
        if (e.kind == TargetKind::DeviceName) {
            if (const DevNodeInfo* dev = inv.FindDevice(e.key)) {
                for (const auto& itf : dev->interfaces) {
                    if (!itf.hasFriendlyName || state.Find(TargetKind::InterfaceName, itf.symbolicLink)) continue;
                    Change c;
                    c.kind = TargetKind::InterfaceName;
                    c.key = itf.symbolicLink;
                    c.group = e.group;
                    c.label = L"Apps: " + itf.friendlyName + L" [" + InterfaceCategoryName(itf.classGuid) +
                              L" interface]";
                    c.value = {true, e.desired};
                    additions.push_back(c);
                }
            }
        }
    }
    if (!additions.empty()) {
        for (auto& r : ApplyChanges(state, additions)) {
            r.note = L"new interface";
            results.push_back(r);
        }
    }
    return results;
}

std::vector<std::wstring> DevicesInChanges(const std::vector<ChangeResult>& results) {
    std::vector<std::wstring> ids;
    for (const auto& r : results) {
        if (r.change.kind != TargetKind::DeviceName || !r.status || r.skipped) continue;
        if (std::none_of(ids.begin(), ids.end(), [&](const std::wstring& id) { return IEquals(id, r.change.key); }))
            ids.push_back(r.change.key);
    }
    return ids;
}

}  // namespace windira
