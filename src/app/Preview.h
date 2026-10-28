// SPDX-License-Identifier: MIT
// "Identify" pane: a live camera picture or a live audio level meter, so two identical
// devices can be told apart before renaming them.
#pragma once

#include "../core/Common.h"

#include <memory>

struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioMeterInformation;

namespace windira {

class PreviewPane {
public:
    PreviewPane() = default;
    ~PreviewPane();

    static void Register(HINSTANCE instance);
    HWND Create(HWND parent, int id, HINSTANCE instance);
    HWND hwnd() const { return hwnd_; }

    void SetFont(HFONT font) { font_ = font; }
    void ShowMessage(const std::wstring& text);

    // symbolicLinks: the device's interface links; the one Media Foundation knows is used.
    void StartCamera(const std::vector<std::wstring>& symbolicLinks);
    Status StartMeter(const std::wstring& endpointId, bool capture);
    void Stop();
    bool Active() const { return camera_ != nullptr || meter_ != nullptr; }

    // True while a camera thread (possibly one abandoned because the camera hung) still runs.
    static bool CameraThreadsRunning();

private:
    struct CameraSession;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC dc, const RECT& rc);
    void PaintFrame(HDC dc, const RECT& rc);
    void PaintMeter(HDC dc, const RECT& rc);
    void OnMeterTimer();
    void OnCameraTimeout();
    void StopMeter();

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    std::wstring message_;
    std::shared_ptr<CameraSession> camera_;

    IAudioMeterInformation* meter_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    float level_ = 0.0f;
    float peakHold_ = 0.0f;
    std::wstring meterLabel_;
};

}  // namespace windira
