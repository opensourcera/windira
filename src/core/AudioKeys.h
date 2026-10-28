// SPDX-License-Identifier: MIT
// Property keys used in the MMDevice (audio endpoint) property store.
#pragma once

#include <windows.h>
#include <wtypes.h>

namespace windira {

// The endpoint name shown everywhere is composed by Windows as
//   "<DeviceDesc> (<InterfaceName>)"   e.g. "Microphone (HD Pro Webcam C920)"
// and both parts live in HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\
// Audio\{Capture|Render}\{endpoint guid}\Properties under these value names.

// "Microphone" - PKEY_Device_DeviceDesc. This is what the Sound control panel renames.
inline const PROPERTYKEY kPKEY_Endpoint_DeviceDesc = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
inline const wchar_t* const kRegValue_Endpoint_DeviceDesc = L"{a45c254e-df1c-4efd-8020-67d146a850e0},2";

// "HD Pro Webcam C920" - the stored device (adapter) part of the endpoint name.
inline const PROPERTYKEY kPKEY_Endpoint_InterfaceName = {
    {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 6};
inline const wchar_t* const kRegValue_Endpoint_InterfaceName = L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6";

// PKEY_Device_ContainerId - links the endpoint to the physical device (webcam, headset...).
inline const PROPERTYKEY kPKEY_Endpoint_ContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

}  // namespace windira
