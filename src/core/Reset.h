// SPDX-License-Identifier: MIT
// "Reset device": restart a device as if it were unplugged and plugged back in, and when
// a program refuses to let go of it, say which one and offer to close it.
#pragma once

#include "Devices.h"

namespace windira {

// A program or Windows service that refused to release a device (Kernel-PnP event 225).
struct Blocker {
    DWORD pid = 0;
    std::wstring imagePath;    // C:\Program Files\Logitech\LogiTune\backend\LogiTuneAgent.exe
    std::wstring fileName;     // LogiTuneAgent.exe
    std::wstring description;  // "Logi Tune Agent" (from the exe's version info), may be empty
    std::vector<std::wstring> services;  // when the process is a service host
    bool canClose = false;     // safe for Windira to close (or, for services, restart)
    std::wstring reason;       // why it cannot be closed

    std::wstring DisplayName() const;
};

struct ResetOutcome {
    enum class Result { Restarted, Blocked, RebootRequired, Failed };
    Result result = Result::Failed;
    std::wstring deviceId;  // the device that was restarted
    std::vector<Blocker> blockers;
    std::wstring message;   // error text for Failed
};

// The device to restart for a selection: a webcam's camera function is restarted through
// its USB composite parent so the whole physical device resets, like a replug.
std::wstring ResetTargetFor(const Inventory& inv, const DevNodeInfo& dev);

// Programs that refused to release deviceId (Kernel-PnP event 225) since a FILETIME value.
std::vector<Blocker> FindBlockers(const std::wstring& deviceId, ULONGLONG sinceFileTime);

// Blocking call: may take several seconds while programs are asked to release the device.
ResetOutcome ResetDevice(const std::wstring& instanceId);

// Closes the closable blockers (ends processes, restarts allowed services).
// Returns a line per blocker describing what happened.
std::vector<std::wstring> CloseBlockers(const std::vector<Blocker>& blockers);

}  // namespace windira
