// SPDX-License-Identifier: MIT
#pragma once

#include "../core/Devices.h"

namespace windira {

struct DeviceRenameRequest {
    const Inventory* inventory = nullptr;
    const DevNodeInfo* device = nullptr;
    std::wstring name;
    bool includeRelated = true;
    bool restart = false;
};

struct EndpointRenameRequest {
    const AudioEndpointInfo* endpoint = nullptr;
    std::wstring description;
    std::wstring deviceName;
};

// Both return true when the user confirmed with valid input.
bool ShowDeviceRenameDialog(HWND owner, DeviceRenameRequest& request);
bool ShowEndpointRenameDialog(HWND owner, EndpointRenameRequest& request);

}  // namespace windira
