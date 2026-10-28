// SPDX-License-Identifier: MIT
// High level rename operations: work out every place a name must change, apply the
// changes with a backup of the original values, restore them, and re-apply them.
#pragma once

#include "Devices.h"
#include "State.h"

namespace windira {

struct Change {
    TargetKind kind = TargetKind::DeviceName;
    std::wstring key;
    std::wstring group;
    std::wstring label;
    TargetValue value;  // value to write (exists == false deletes it)
};

struct ChangeResult {
    Change change;
    Status status;
    bool skipped = false;  // nothing to do (already correct, or device not connected)
    std::wstring note;
};

// Renaming a device changes its Device Manager name and every named interface of it.
// With includeRelated, other parts of the same physical device (for a webcam: its
// audio function, composite USB parent and the microphone endpoint) get the name too.
std::vector<Change> PlanDeviceRename(const Inventory& inv, const DevNodeInfo& dev, const std::wstring& name,
                                     bool includeRelated);

// Either part may be null to leave it unchanged.
std::vector<Change> PlanEndpointRename(const AudioEndpointInfo& ep, const std::wstring* description,
                                       const std::wstring* deviceName);

std::vector<ChangeResult> ApplyChanges(StateStore& state, const std::vector<Change>& changes);

// Restores the original names of a device or endpoint and everything renamed with it.
std::vector<ChangeResult> Restore(StateStore& state, const std::wstring& groupOrKey);
std::vector<ChangeResult> RestoreAll(StateStore& state);

// Writes the desired names again wherever Windows has reset them (driver update,
// device reinstall). Also names interfaces that appeared since the rename.
std::vector<ChangeResult> Reapply(StateStore& state, const Inventory& inv);

// Devices whose names Windira touched for a given group, used to restart them.
std::vector<std::wstring> DevicesInChanges(const std::vector<ChangeResult>& results);

std::wstring InterfaceCategoryName(const std::wstring& classGuid);

}  // namespace windira
