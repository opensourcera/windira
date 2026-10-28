// SPDX-License-Identifier: MIT
#pragma once

#include "../core/Devices.h"
#include "../core/Renamer.h"
#include "../core/Reset.h"
#include "../core/State.h"
#include "Preview.h"

namespace windira {

struct ResetJob;  // result of a background device reset (MainWindow.cpp)

class MainWindow {
public:
    bool Create(HINSTANCE instance, int showCommand);
    HWND hwnd() const { return hwnd_; }
    bool PreTranslate(MSG* msg);

private:
    struct Row {
        bool endpoint = false;
        size_t index = 0;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    void ApplyDpi();
    void SetupColumns();
    void Layout();
    int Scale(int px) const { return MulDiv(px, dpi_, 96); }

    void Reload(const std::wstring& selectKey);
    void Populate(const std::wstring& selectKey);
    void OnSelectionChanged();
    void UpdateButtons();
    void UpdateStatus(const std::wstring& message = L"");

    void OnRename();
    void OnRestore();
    void OnIdentify();
    void OnToggleTask();
    void OnReset();
    void StartReset(const std::wstring& target, const std::wstring& selectKey, bool retry,
                    const std::vector<Blocker>& closeFirst);
    void OnResetDone(ResetJob* job);
    std::wstring ResetTarget() const;
    void ReportResults(const std::wstring& action, const std::vector<ChangeResult>& results);
    void ShowAbout();
    bool EnsureElevated();  // offers to relaunch elevated; true when already elevated

    const Row* SelectedRow() const;
    const DevNodeInfo* SelectedDevice() const;
    const AudioEndpointInfo* SelectedEndpoint() const;
    std::wstring SelectedKey() const;
    std::wstring DetailsText() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND search_ = nullptr, showAll_ = nullptr, refresh_ = nullptr, list_ = nullptr, details_ = nullptr;
    HWND rename_ = nullptr, restore_ = nullptr, identify_ = nullptr, reset_ = nullptr;
    HWND autoTask_ = nullptr, status_ = nullptr;
    HACCEL accel_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;

    PreviewPane preview_;
    Inventory inventory_;
    StateStore state_;
    std::vector<Row> rows_;
    bool elevated_ = false;
    bool taskInstalled_ = false;
    bool resetting_ = false;
};

}  // namespace windira
