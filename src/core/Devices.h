// SPDX-License-Identifier: MIT
// Enumeration of Plug and Play device nodes, their device interfaces and audio endpoints.
#pragma once

#include "Common.h"

namespace windira {

// One device interface (e.g. a camera's KSCATEGORY_VIDEO_CAMERA interface).
// Camera apps (DirectShow / Media Foundation) read the name from the interface's
// "Device Parameters\FriendlyName" registry value, not from the device node.
struct DeviceInterfaceInfo {
    std::wstring symbolicLink;   // \\?\USB#VID_046D&PID_08E5&MI_00#...#{guid}\GLOBAL
    std::wstring classGuid;      // {e5323777-...}
    std::wstring friendlyName;
    bool hasFriendlyName = false;
};

// One Plug and Play device node, as shown in Device Manager.
struct DevNodeInfo {
    std::wstring instanceId;     // USB\VID_046D&PID_08E5&MI_00\8&285CC37B&0&0000
    std::wstring friendlyName;   // SPDRP_FRIENDLYNAME, may be absent
    bool hasFriendlyName = false;
    std::wstring deviceDesc;     // SPDRP_DEVICEDESC, fallback display name
    std::wstring className;      // Camera, MEDIA, USB, ...
    std::wstring classGuid;
    std::wstring manufacturer;
    std::wstring locationInfo;   // "Port_#0002.Hub_#0003"
    std::wstring locationPath;   // "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(3)"
    std::wstring containerId;    // physical device grouping
    std::wstring parentId;
    std::wstring hardwareId;     // first hardware id
    std::wstring serial;         // USB serial number when the device reports one
    std::vector<DeviceInterfaceInfo> interfaces;

    const std::wstring& DisplayName() const {
        return hasFriendlyName && !friendlyName.empty() ? friendlyName : deviceDesc;
    }
    // Interfaces whose name apps actually read.
    size_t NamedInterfaceCount() const;
};

// One audio endpoint from the Windows Sound settings, e.g. "Microphone (HD Pro Webcam C920)".
// Its full name is "<deviceDesc> (<interfaceName>)".
struct AudioEndpointInfo {
    std::wstring id;             // {0.0.1.00000000}.{4CFD0F82-...}
    bool capture = false;        // true = recording device, false = playback
    std::wstring deviceDesc;     // "Microphone"
    std::wstring interfaceName;  // "HD Pro Webcam C920"
    std::wstring friendlyName;   // "Microphone (HD Pro Webcam C920)"
    std::wstring containerId;
    DWORD state = 0;             // DEVICE_STATE_*
};

struct Inventory {
    std::vector<DevNodeInfo> devices;
    std::vector<AudioEndpointInfo> endpoints;

    // Loads every present device node and every active audio endpoint.
    Status Load();

    const DevNodeInfo* FindDevice(const std::wstring& instanceId) const;
    const AudioEndpointInfo* FindEndpoint(const std::wstring& id) const;
};

// Classes shown by default in the UI because their names surface in everyday apps.
bool IsMediaClass(const std::wstring& className);

Status EnumerateDevNodes(std::vector<DevNodeInfo>& out);
Status EnumerateAudioEndpoints(std::vector<AudioEndpointInfo>& out);

}  // namespace windira
