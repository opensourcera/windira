// SPDX-License-Identifier: MIT
// Low-level read/write of every place Windows stores a device name.
#pragma once

#include "Common.h"

namespace windira {

enum class TargetKind {
    DeviceName,           // device node FriendlyName (Device Manager, Settings)
    InterfaceName,        // device interface Device Parameters\FriendlyName (camera apps)
    EndpointDescription,  // audio endpoint "Microphone" part
    EndpointDeviceName,   // audio endpoint "(HD Pro Webcam C920)" part
};

const wchar_t* TargetKindId(TargetKind kind);     // stable id stored in the state file
const wchar_t* TargetKindLabel(TargetKind kind);  // human readable
bool ParseTargetKind(const std::wstring& id, TargetKind& out);

struct TargetValue {
    bool exists = false;
    std::wstring value;
};

// key = device instance id, interface symbolic link or audio endpoint id, depending on kind.
Status ReadTarget(TargetKind kind, const std::wstring& key, TargetValue& out);
// value.exists == false removes the value so Windows falls back to its default name.
Status WriteTarget(TargetKind kind, const std::wstring& key, const TargetValue& value);

bool ReadInterfaceFriendlyName(const std::wstring& link, std::wstring& out);

// Restarts a single device (like disable + enable) so drivers re-read their names.
// Sets rebootRequired when Windows says the device cannot be restarted live.
Status RestartDevice(const std::wstring& instanceId, bool& rebootRequired);

}  // namespace windira
