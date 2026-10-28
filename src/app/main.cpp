// SPDX-License-Identifier: MIT
// windira.exe - graphical device renamer.
#include "../core/Common.h"

#include <commctrl.h>
#include <mfapi.h>

#include "MainWindow.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    windira::ComInit com(COINIT_APARTMENTTHREADED);
    bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    windira::MainWindow window;
    int code = 1;
    if (window.Create(instance, showCommand)) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (window.PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        code = (int)msg.wParam;
    }
    // A camera stuck in its driver can block MFShutdown forever; Windows releases it at exit.
    if (mf && !windira::PreviewPane::CameraThreadsRunning()) MFShutdown();
    return code;
}
