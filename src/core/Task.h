// SPDX-License-Identifier: MIT
// Optional Task Scheduler task that re-applies names after driver installs/updates.
// It runs "windira-cli.exe reapply" as SYSTEM at boot and whenever Kernel-PnP logs that a
// device was configured (event 400) or started (event 410). No resident service needed.
#pragma once

#include "Common.h"

namespace windira {

constexpr const wchar_t* kTaskName = L"\\Windira\\Reapply device names";

bool IsReapplyTaskInstalled();
// Copies sourceCliPath to %ProgramFiles%\Windira (admin-only) and registers the task there.
Status InstallReapplyTask(const std::wstring& sourceCliPath);
Status RemoveReapplyTask();

// Path of windira-cli.exe next to the running executable.
std::wstring DefaultCliPath();

}  // namespace windira
