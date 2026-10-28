// SPDX-License-Identifier: MIT
// Persistent record of every rename: the original value (for restore) and the
// desired value (for re-applying after driver updates).
#pragma once

#include "Common.h"
#include "Targets.h"

namespace windira {

struct RenameEntry {
    TargetKind kind = TargetKind::DeviceName;
    std::wstring key;      // instance id, interface symbolic link or endpoint id
    std::wstring group;    // the device or endpoint the user renamed (restore unit)
    std::wstring label;    // human readable description of this target
    TargetValue original;  // value before Windira touched it
    std::wstring desired;  // value Windira keeps applying
    std::wstring updated;  // ISO 8601 UTC timestamp
};

class StateStore {
public:
    static std::wstring DefaultPath();  // %ProgramData%\Windira\renames.json

    // A missing file is not an error: it just means nothing has been renamed yet.
    Status Load(const std::wstring& path = DefaultPath());
    Status Save() const;

    std::vector<RenameEntry>& entries() { return entries_; }
    const std::vector<RenameEntry>& entries() const { return entries_; }
    const std::wstring& path() const { return path_; }

    RenameEntry* Find(TargetKind kind, const std::wstring& key);
    void Remove(TargetKind kind, const std::wstring& key);
    // True when the device or endpoint (or anything renamed together with it) is tracked.
    bool IsTracked(const std::wstring& groupOrKey) const;

private:
    std::wstring path_;
    std::vector<RenameEntry> entries_;
};

std::wstring NowIso8601();

}  // namespace windira
