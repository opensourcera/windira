// SPDX-License-Identifier: MIT
#include "Preview.h"

#include <audioclient.h>
#include <endpointvolume.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace windira {

namespace {

constexpr wchar_t kClassName[] = L"WindiraPreview";
constexpr UINT WM_APP_FRAME = WM_APP + 10;
constexpr UINT_PTR kMeterTimer = 1;
constexpr UINT_PTR kCameraTimeoutTimer = 2;
constexpr UINT kCameraTimeoutMs = 8000;
const wchar_t kStartingCamera[] = L"Starting camera…";

std::atomic<int> g_liveCameraThreads{0};

const COLORREF kBackground = RGB(28, 30, 34);
const COLORREF kText = RGB(225, 228, 232);
const COLORREF kMuted = RGB(150, 155, 162);

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring CameraError(HRESULT hr, const wchar_t* step) {
    if (hr == E_ACCESSDENIED)
        return L"Windows blocked camera access. Check Settings > Privacy & security > Camera "
               L"(\"Let desktop apps access your camera\").";
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED || hr == MF_E_HW_MFT_FAILED_START_STREAMING ||
        hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == (HRESULT)0xC00D3704)
        return L"The camera is in use by another app. Close it and try again.";
    if (wcscmp(step, L"reading frames") == 0)
        return L"The camera did not send a picture. It may be busy in another app; unplugging and reconnecting "
               L"it usually helps. (" + HResultMessage(hr) + L")";
    return std::wstring(L"The camera failed while ") + step + L": " + HResultMessage(hr);
}

}  // namespace

struct PreviewPane::CameraSession {
    HWND hwnd = nullptr;
    std::mutex lock;
    std::vector<BYTE> pixels;  // top-down BGRA
    UINT32 width = 0, height = 0;
    std::wstring status;
    IMFMediaSource* source = nullptr;  // guarded by lock, used to unblock ReadSample
    std::atomic<bool> stop{false};
    const wchar_t* step = L"starting";

    // Called on a throwaway thread: a camera stuck in its driver or in another app can
    // block Shutdown (and every other Media Foundation call) indefinitely.
    void Shutdown() {
        IMFMediaSource* src = nullptr;
        {
            std::lock_guard<std::mutex> g(lock);
            stop = true;
            src = source;
            if (src) src->AddRef();
        }
        if (src) {
            src->Shutdown();  // unblocks a pending ReadSample
            src->Release();
        }
    }

    void SetStatus(const std::wstring& text) {
        if (stop) return;
        {
            std::lock_guard<std::mutex> g(lock);
            status = text;
        }
        PostMessageW(hwnd, WM_APP_FRAME, 0, 0);
    }

    void Run(std::vector<std::wstring> links) {
        ++g_liveCameraThreads;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = Capture(links);
        if (FAILED(hr) && !stop) SetStatus(CameraError(hr, step));
        CoUninitialize();
        --g_liveCameraThreads;
    }

    HRESULT Capture(const std::vector<std::wstring>& links) {
        step = L"listing cameras";
        IMFAttributes* attrs = nullptr;
        HRESULT hr = MFCreateAttributes(&attrs, 1);
        if (FAILED(hr)) return hr;
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attrs, &devices, &count);
        attrs->Release();
        if (FAILED(hr)) return hr;

        IMFActivate* match = nullptr;
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* link = nullptr;
            UINT32 len = 0;
            if (!match && SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                                   &link, &len))) {
                for (const auto& l : links) {
                    if (IEquals(l, link)) {
                        match = devices[i];
                        match->AddRef();
                        break;
                    }
                }
                CoTaskMemFree(link);
            }
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
        if (!match) {
            SetStatus(L"Windows does not offer a camera picture for this device.");
            return S_OK;
        }

        IMFMediaSource* src = nullptr;
        step = L"opening the camera";
        hr = match->ActivateObject(IID_PPV_ARGS(&src));
        match->Release();
        if (FAILED(hr)) return hr;
        {
            std::lock_guard<std::mutex> g(lock);
            if (stop) {
                src->Shutdown();
                src->Release();
                return S_OK;
            }
            source = src;
        }

        const DWORD kStream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
        IMFSourceReader* reader = nullptr;
        IMFAttributes* readerAttrs = nullptr;
        step = L"creating the frame reader";
        hr = MFCreateAttributes(&readerAttrs, 1);
        if (SUCCEEDED(hr)) {
            // Lets the reader decode and convert MJPG/YUY2/NV12 camera output to RGB32 for us.
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            hr = MFCreateSourceReaderFromMediaSource(src, readerAttrs, &reader);
            readerAttrs->Release();
        }
        if (SUCCEEDED(hr)) {
            // Pick a modest native mode (about 640 px wide, uncompressed if possible):
            // a preview does not need full HD and this keeps CPU use low.
            step = L"choosing a video mode";
            IMFMediaType* best = nullptr;
            UINT32 bestScore = UINT32_MAX;
            IMFMediaType* native = nullptr;
            for (DWORD i = 0; SUCCEEDED(reader->GetNativeMediaType(kStream, i, &native)); ++i) {
                GUID subtype{};
                UINT32 tw = 0, th = 0;
                native->GetGUID(MF_MT_SUBTYPE, &subtype);
                MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &tw, &th);
                bool uncompressed = subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2 ||
                                    subtype == MFVideoFormat_RGB32;
                UINT32 score = (UINT32)std::abs((int)tw - 640) + (uncompressed ? 0 : 5000);
                if (tw && th && score < bestScore) {
                    SafeRelease(best);
                    best = native;
                    bestScore = score;
                } else {
                    native->Release();
                }
                native = nullptr;
            }
            if (best) {
                reader->SetCurrentMediaType(kStream, nullptr, best);  // best effort; defaults are fine too
                best->Release();
            }
            step = L"converting the picture";
            IMFMediaType* type = nullptr;
            hr = MFCreateMediaType(&type);
            if (SUCCEEDED(hr)) {
                type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                hr = reader->SetCurrentMediaType(kStream, nullptr, type);
                type->Release();
            }
        }
        LONG stride = 0;
        UINT32 w = 0, h = 0;
        auto readFormat = [&]() {
            IMFMediaType* current = nullptr;
            if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current))) {
                MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
                stride = (LONG)MFGetAttributeUINT32(current, MF_MT_DEFAULT_STRIDE, w * 4);
                current->Release();
            }
        };
        if (SUCCEEDED(hr)) readFormat();

        step = L"reading frames";
        while (SUCCEEDED(hr) && !stop) {
            DWORD stream = 0, flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, &sample);
            if (FAILED(hr) || stop) {
                SafeRelease(sample);
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) readFormat();
            if (!sample || !w || !h) {
                SafeRelease(sample);
                continue;
            }
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                IMF2DBuffer* buffer2d = nullptr;
                BYTE* raw = nullptr;
                bool locked2d = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) &&
                                SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
                if (!locked2d) {
                    DWORD maxLen = 0, curLen = 0;
                    if (SUCCEEDED(buffer->Lock(&raw, &maxLen, &curLen))) {
                        pitch = stride ? stride : (LONG)w * 4;
                        // A negative stride means the image is stored bottom-up.
                        scan0 = pitch < 0 ? raw + (size_t)(h - 1) * (size_t)(-pitch) : raw;
                        if ((size_t)curLen < (size_t)h * (size_t)std::abs(pitch)) scan0 = nullptr;
                    }
                }
                if (scan0) {
                    std::lock_guard<std::mutex> g(lock);
                    width = w;
                    height = h;
                    status.clear();
                    pixels.resize((size_t)w * h * 4);
                    for (UINT32 y = 0; y < h; ++y)
                        memcpy(&pixels[(size_t)y * w * 4], scan0 + (ptrdiff_t)y * pitch, (size_t)w * 4);
                }
                if (locked2d) buffer2d->Unlock2D();
                else if (raw) buffer->Unlock();
                SafeRelease(buffer2d);
                buffer->Release();
                if (!stop) PostMessageW(hwnd, WM_APP_FRAME, 0, 0);
            }
            sample->Release();
        }

        SafeRelease(reader);
        {
            std::lock_guard<std::mutex> g(lock);
            source = nullptr;
        }
        src->Shutdown();
        src->Release();
        return stop ? S_OK : hr;
    }
};

PreviewPane::~PreviewPane() {
    Stop();
}

void PreviewPane::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND PreviewPane::Create(HWND parent, int id, HINSTANCE instance) {
    hwnd_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent,
                            (HMENU)(INT_PTR)id, instance, this);
    return hwnd_;
}

void PreviewPane::ShowMessage(const std::wstring& text) {
    message_ = text;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewPane::StartCamera(const std::vector<std::wstring>& symbolicLinks) {
    Stop();
    camera_ = std::make_shared<CameraSession>();
    camera_->hwnd = hwnd_;
    camera_->status = kStartingCamera;
    auto session = camera_;
    // Detached: the thread keeps the session alive, and the UI never waits for it.
    std::thread([session, symbolicLinks]() { session->Run(symbolicLinks); }).detach();
    SetTimer(hwnd_, kCameraTimeoutTimer, kCameraTimeoutMs, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewPane::OnCameraTimeout() {
    KillTimer(hwnd_, kCameraTimeoutTimer);
    if (!camera_) return;
    {
        std::lock_guard<std::mutex> g(camera_->lock);
        if (!camera_->pixels.empty() || camera_->status != kStartingCamera) return;
        camera_->status =
            L"The camera is not responding. Another app may be stuck holding it (check Task Manager for "
            L"a \"Not responding\" camera app), or unplug the camera and plug it back in.";
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool PreviewPane::CameraThreadsRunning() {
    return g_liveCameraThreads > 0;
}

Status PreviewPane::StartMeter(const std::wstring& endpointId, bool capture) {
    Stop();
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(endpointId.c_str(), &device);
    SafeRelease(enumerator);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter_);
    if (SUCCEEDED(hr) && capture) {
        // A recording device only reports levels while something records from it.
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
        WAVEFORMATEX* format = nullptr;
        if (SUCCEEDED(hr)) hr = audioClient_->GetMixFormat(&format);
        if (SUCCEEDED(hr)) hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, format, nullptr);
        if (format) CoTaskMemFree(format);
        if (SUCCEEDED(hr)) hr = audioClient_->GetService(IID_PPV_ARGS(&captureClient_));
        if (SUCCEEDED(hr)) hr = audioClient_->Start();
    }
    SafeRelease(device);
    if (FAILED(hr)) {
        StopMeter();
        std::wstring msg = hr == E_ACCESSDENIED
                               ? L"Windows blocked microphone access. Check Settings > Privacy & security > Microphone."
                               : L"Cannot open this audio device: " + HResultMessage(hr);
        ShowMessage(msg);
        return Status::Fail(msg);
    }
    meterLabel_ = capture ? L"Speak or tap the microphone - the bar moves for the device you selected."
                          : L"Play a sound - the bar moves for the device you selected.";
    level_ = peakHold_ = 0;
    SetTimer(hwnd_, kMeterTimer, 40, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return Status::Ok();
}

void PreviewPane::StopMeter() {
    if (hwnd_) KillTimer(hwnd_, kMeterTimer);
    if (audioClient_) audioClient_->Stop();
    SafeRelease(captureClient_);
    SafeRelease(audioClient_);
    SafeRelease(meter_);
}

void PreviewPane::Stop() {
    if (camera_) {
        // Shut the camera down off the UI thread; a stuck camera must not freeze the window.
        auto session = camera_;
        session->stop = true;
        std::thread([session]() { session->Shutdown(); }).detach();
        camera_.reset();
    }
    if (hwnd_) KillTimer(hwnd_, kCameraTimeoutTimer);
    StopMeter();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewPane::OnMeterTimer() {
    if (!meter_) return;
    if (captureClient_) {
        UINT32 packet = 0;
        while (SUCCEEDED(captureClient_->GetNextPacketSize(&packet)) && packet) {
            BYTE* data;
            UINT32 frames;
            DWORD flags;
            if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            captureClient_->ReleaseBuffer(frames);
        }
    }
    float peak = 0;
    meter_->GetPeakValue(&peak);
    float perceived = std::sqrt(std::clamp(peak, 0.0f, 1.0f));
    level_ = std::max(perceived, level_ * 0.82f);
    peakHold_ = std::max(perceived, peakHold_ - 0.01f);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewPane::PaintFrame(HDC dc, const RECT& rc) {
    std::lock_guard<std::mutex> g(camera_->lock);
    if (!camera_->status.empty() || camera_->pixels.empty()) {
        RECT text = rc;
        InflateRect(&text, -12, -12);
        SetTextColor(dc, kText);
        DrawTextW(dc, camera_->status.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    double scale = std::min((double)cw / camera_->width, (double)ch / camera_->height);
    int dw = (int)(camera_->width * scale), dh = (int)(camera_->height * scale);
    int dx = rc.left + (cw - dw) / 2, dy = rc.top + (ch - dh) / 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)camera_->width;
    bmi.bmiHeader.biHeight = -(LONG)camera_->height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, camera_->width, camera_->height, camera_->pixels.data(), &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
}

void PreviewPane::PaintMeter(HDC dc, const RECT& rc) {
    int pad = std::max(12, (int)(rc.right - rc.left) / 20);
    RECT text{rc.left + pad, rc.top + pad, rc.right - pad, rc.top + (rc.bottom - rc.top) / 2 - 8};
    RECT measure = text;
    DrawTextW(dc, meterLabel_.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    text.top = std::max(text.top, text.bottom - (measure.bottom - measure.top));  // bottom-align above the bar
    SetTextColor(dc, kText);
    DrawTextW(dc, meterLabel_.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK);

    int barH = std::max(14, (int)(rc.bottom - rc.top) / 10);
    RECT bar{rc.left + pad, rc.top + (rc.bottom - rc.top) / 2 + 4, rc.right - pad, 0};
    bar.bottom = bar.top + barH;
    HBRUSH track = CreateSolidBrush(RGB(52, 56, 62));
    FillRect(dc, &bar, track);
    DeleteObject(track);

    RECT fill = bar;
    fill.right = bar.left + (LONG)((bar.right - bar.left) * level_);
    COLORREF color = level_ > 0.85f ? RGB(239, 68, 68) : level_ > 0.6f ? RGB(234, 179, 8) : RGB(34, 197, 94);
    HBRUSH fillBrush = CreateSolidBrush(color);
    FillRect(dc, &fill, fillBrush);
    DeleteObject(fillBrush);

    LONG px = bar.left + (LONG)((bar.right - bar.left) * peakHold_);
    RECT peak{std::max(bar.left, px - 2), bar.top, std::min(bar.right, px + 1), bar.bottom};
    HBRUSH peakBrush = CreateSolidBrush(kText);
    FillRect(dc, &peak, peakBrush);
    DeleteObject(peakBrush);
}

void PreviewPane::Paint(HDC dc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    if (camera_) {
        PaintFrame(dc, rc);
    } else if (meter_) {
        PaintMeter(dc, rc);
    } else {
        RECT text = rc;
        InflateRect(&text, -16, -16);
        SetTextColor(dc, kMuted);
        RECT measure = text;
        DrawTextW(dc, message_.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        text.top += ((text.bottom - text.top) - (measure.bottom - measure.top)) / 2;
        DrawTextW(dc, message_.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK);
    }
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK PreviewPane::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PreviewPane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // Double buffer to avoid flicker at camera frame rate.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            if (self) self->Paint(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (self && wp == kMeterTimer) self->OnMeterTimer();
            if (self && wp == kCameraTimeoutTimer) self->OnCameraTimeout();
            return 0;
        case WM_APP_FRAME:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            if (self) {
                self->Stop();
                self->hwnd_ = nullptr;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace windira
