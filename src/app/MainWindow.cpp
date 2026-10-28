// SPDX-License-Identifier: MIT
#include "MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <map>
#include <memory>
#include <thread>

#include "../core/Task.h"
#include "RenameDialog.h"
#include "resource.h"

namespace windira {

namespace {

constexpr wchar_t kClassName[] = L"WindiraMainWindow";
constexpr UINT WM_APP_RESET_DONE = WM_APP + 20;

enum ControlId {
    IDC_SEARCH = 2001,
    IDC_SHOWALL,
    IDC_REFRESH,
    IDC_LIST,
    IDC_PREVIEW,
    IDC_DETAILS,
    IDC_RENAME,
    IDC_RESTORE,
    IDC_IDENTIFY,
    IDC_RESET,
    IDC_AUTOTASK,
    IDC_STATUS,
    IDM_FOCUS_SEARCH,
    IDM_ABOUT = 0x0010,  // system menu ids must be multiples of 16
};

enum GroupId {
    kGroupCameras = 1,
    kGroupRecording,
    kGroupPlayback,
    kGroupMedia,
    kGroupImaging,
    kGroupOtherBase = 100,
};

enum Column { kColName, kColType, kColConnection, kColSerial, kColStatus };

const wchar_t kIdleHint[] = L"Select a camera or microphone, then click Identify to see which physical device it is.";

std::wstring TypeLabel(const std::wstring& className) {
    if (IEquals(className, L"Camera")) return L"Camera";
    if (IEquals(className, L"MEDIA")) return L"Audio / video";
    if (IEquals(className, L"Image")) return L"Imaging";
    return className;
}

// "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(7)#USB(1)#USBMI(0)" -> "USB port 7 › 1"
std::wstring Connection(const DevNodeInfo& d) {
    const std::wstring& path = d.locationPath;
    size_t root = path.find(L"USBROOT(");
    if (root != std::wstring::npos) {
        std::wstring ports;
        for (size_t pos = path.find(L"#USB(", root); pos != std::wstring::npos; pos = path.find(L"#USB(", pos + 1)) {
            size_t close = path.find(L')', pos);
            if (close == std::wstring::npos) break;
            if (!ports.empty()) ports += L" › ";
            ports += path.substr(pos + 5, close - pos - 5);
        }
        if (!ports.empty()) return L"USB port " + ports;
    }
    if (IContains(d.instanceId.substr(0, 8), L"BTH")) return L"Bluetooth";
    return d.locationInfo;
}

std::wstring GetWindowString(HWND hwnd) {
    std::wstring text(GetWindowTextLengthW(hwnd) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), (int)text.size());
    text.resize(wcslen(text.c_str()));
    return text;
}

}  // namespace

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
    instance_ = instance;
    elevated_ = IsElevated();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    PreviewPane::Register(instance);

    hwnd_ = CreateWindowExW(0, kClassName, L"Windira - Device Renamer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    // Size the window for the monitor's DPI now that we know it.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::min(Scale(1180), (int)(work.right - work.left));
    int h = std::min(Scale(720), (int)(work.bottom - work.top));
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER);

    ACCEL accels[] = {
        {FVIRTKEY, VK_F2, IDC_RENAME},
        {FVIRTKEY, VK_F5, IDC_REFRESH},
        {FVIRTKEY | FCONTROL, 'F', IDM_FOCUS_SEARCH},
    };
    accel_ = CreateAcceleratorTableW(accels, ARRAYSIZE(accels));

    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    taskInstalled_ = IsReapplyTaskInstalled();
    Button_SetCheck(autoTask_, taskInstalled_ ? BST_CHECKED : BST_UNCHECKED);
    Reload(L"");
    SetCursor(old);
    SetFocus(list_);
    return true;
}

bool MainWindow::PreTranslate(MSG* msg) {
    if (accel_ && TranslateAcceleratorW(hwnd_, accel_, msg)) return true;
    return IsDialogMessageW(hwnd_, msg) != FALSE;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            dpi_ = (int)GetDpiForWindow(hwnd_);
            CreateControls();
            ApplyDpi();
            return 0;

        case WM_SIZE:
            Layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {Scale(900), Scale(500)};
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            ApplyDpi();
            auto* rc = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_SEARCH:
                    if (HIWORD(wp) == EN_CHANGE) Populate(SelectedKey());
                    return 0;
                case IDC_SHOWALL:
                    Populate(SelectedKey());
                    return 0;
                case IDC_REFRESH:
                    Reload(SelectedKey());
                    return 0;
                case IDC_RENAME:
                    OnRename();
                    return 0;
                case IDC_RESTORE:
                    OnRestore();
                    return 0;
                case IDC_IDENTIFY:
                    OnIdentify();
                    return 0;
                case IDC_RESET:
                    OnReset();
                    return 0;
                case IDC_AUTOTASK:
                    OnToggleTask();
                    return 0;
                case IDM_FOCUS_SEARCH:
                    SetFocus(search_);
                    Edit_SetSel(search_, 0, -1);
                    return 0;
            }
            break;

        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == IDM_ABOUT) {
                ShowAbout();
                return 0;
            }
            break;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == IDC_LIST) {
                if (hdr->code == LVN_ITEMCHANGED) {
                    auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                    if ((nm->uChanged & LVIF_STATE) && ((nm->uOldState ^ nm->uNewState) & LVIS_SELECTED))
                        OnSelectionChanged();
                } else if (hdr->code == NM_DBLCLK) {
                    OnRename();
                }
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
            // Checkboxes and the read-only details box sit on the white window background.
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

        case WM_APP_RESET_DONE:
            OnResetDone(reinterpret_cast<ResetJob*>(lp));
            return 0;

        case WM_DESTROY:
            preview_.Stop();
            if (font_) DeleteObject(font_);
            if (accel_) DestroyAcceleratorTable(accel_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::CreateControls() {
    auto make = [&](DWORD exStyle, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               (HMENU)(INT_PTR)id, instance_, nullptr);
    };
    search_ = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_SEARCH);
    Edit_SetCueBannerTextFocused(search_, L"Search devices (Ctrl+F)", TRUE);
    showAll_ = make(0, WC_BUTTONW, L"Show all device types", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_SHOWALL);
    refresh_ = make(0, WC_BUTTONW, L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, IDC_REFRESH);

    list_ = make(0, WC_LISTVIEWW, L"",
                 WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, IDC_LIST);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_EnableGroupView(list_, TRUE);

    preview_.Create(hwnd_, IDC_PREVIEW, instance_);
    preview_.ShowMessage(kIdleHint);
    details_ = make(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                    WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_DETAILS);

    rename_ = make(0, WC_BUTTONW, L"&Rename…", WS_TABSTOP | BS_PUSHBUTTON, IDC_RENAME);
    restore_ = make(0, WC_BUTTONW, L"Restore &original", WS_TABSTOP | BS_PUSHBUTTON, IDC_RESTORE);
    identify_ = make(0, WC_BUTTONW, L"&Identify", WS_TABSTOP | BS_PUSHBUTTON, IDC_IDENTIFY);
    reset_ = make(0, WC_BUTTONW, L"Rese&t device", WS_TABSTOP | BS_PUSHBUTTON, IDC_RESET);
    autoTask_ = make(0, WC_BUTTONW, L"&Keep my names after driver updates", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_AUTOTASK);
    status_ = make(0, STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, IDC_STATUS);

    HMENU sys = GetSystemMenu(hwnd_, FALSE);
    AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sys, MF_STRING, IDM_ABOUT, L"About Windira…");
}

void MainWindow::ApplyDpi() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {search_, showAll_, refresh_, list_, details_, rename_, restore_, identify_, reset_, autoTask_,
                   status_})
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    preview_.SetFont(font);
    if (font_) DeleteObject(font_);
    font_ = font;
    SetupColumns();
    Layout();
}

void MainWindow::SetupColumns() {
    struct ColumnDef {
        const wchar_t* title;
        int width;
    };
    const ColumnDef cols[] = {{L"Name", 300}, {L"Type", 110}, {L"Connection", 140}, {L"Serial", 110}, {L"Status", 90}};
    HWND header = ListView_GetHeader(list_);
    int existing = header ? Header_GetItemCount(header) : 0;
    for (int i = 0; i < (int)ARRAYSIZE(cols); ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(cols[i].title);
        col.cx = Scale(cols[i].width);
        if (i < existing) ListView_SetColumn(list_, i, &col);
        else ListView_InsertColumn(list_, i, &col);
    }
}

void MainWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right == 0) return;
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(status_, &sb);
    int statusH = sb.bottom - sb.top;

    const int m = Scale(12), gap = Scale(8), rowH = Scale(26), btnW = Scale(118);
    const int panelW = std::max(Scale(300), std::min(Scale(420), (int)(rc.right / 3)));
    int bottom = rc.bottom - statusH - m;

    HDWP dwp = BeginDeferWindowPos(12);
    auto place = [&](HWND h, int x, int y, int w, int hgt) {
        dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(0, w), std::max(0, hgt), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    // Top row: search, filter, refresh
    place(search_, m, m, Scale(300), rowH);
    place(showAll_, m + Scale(300) + gap * 2, m, Scale(200), rowH);
    place(refresh_, rc.right - m - Scale(96), m, Scale(96), rowH);

    // Bottom row: actions on the left, the auto re-apply switch on the right
    int by = bottom - rowH;
    place(rename_, m, by, btnW, rowH);
    place(restore_, m + btnW + gap, by, btnW, rowH);
    place(identify_, m + (btnW + gap) * 2, by, btnW, rowH);
    place(reset_, m + (btnW + gap) * 3, by, btnW, rowH);
    place(autoTask_, rc.right - m - Scale(290), by, Scale(290), rowH);

    // Middle: list on the left, preview + details on the right
    int top = m + rowH + gap;
    int midH = by - gap - top;
    int listW = rc.right - m * 2 - gap - panelW;
    place(list_, m, top, listW, midH);
    int px = m + listW + gap;
    int previewH = std::min(panelW * 3 / 4, midH / 2);
    place(preview_.hwnd(), px, top, panelW, previewH);
    place(details_, px, top + previewH + gap, panelW, midH - previewH - gap);
    EndDeferWindowPos(dwp);
}

void MainWindow::Reload(const std::wstring& selectKey) {
    preview_.Stop();
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    Status s = state_.Load();
    if (!s) MessageBoxW(hwnd_, s.message.c_str(), L"Windira", MB_ICONWARNING);
    s = inventory_.Load();
    if (!s) MessageBoxW(hwnd_, s.message.c_str(), L"Windira", MB_ICONERROR);
    SetCursor(old);
    Populate(selectKey);
}

void MainWindow::Populate(const std::wstring& selectKey) {
    std::wstring filter = Trim(GetWindowString(search_));
    bool all = Button_GetCheck(showAll_) == BST_CHECKED;

    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    ListView_RemoveAllGroups(list_);
    rows_.clear();

    auto addGroup = [&](int id, const std::wstring& title) {
        LVGROUP g{sizeof(g)};
        g.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        g.pszHeader = const_cast<LPWSTR>(title.c_str());
        g.iGroupId = id;
        g.stateMask = LVGS_COLLAPSIBLE;
        g.state = LVGS_COLLAPSIBLE;
        ListView_InsertGroup(list_, -1, &g);
    };
    addGroup(kGroupCameras, L"Cameras");
    addGroup(kGroupRecording, L"Recording devices (microphones)");
    addGroup(kGroupPlayback, L"Playback devices (speakers, headphones, monitors)");
    addGroup(kGroupMedia, L"Sound, video and game controllers");
    addGroup(kGroupImaging, L"Imaging devices");
    std::map<std::wstring, int> otherGroups;

    int selectIndex = -1;
    auto addItem = [&](const Row& row, int group, const std::wstring& name, const std::wstring& type,
                       const std::wstring& connection, const std::wstring& serial, const std::wstring& status,
                       const std::wstring& key) {
        rows_.push_back(row);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
        item.iItem = ListView_GetItemCount(list_);
        item.pszText = const_cast<LPWSTR>(name.c_str());
        item.lParam = (LPARAM)(rows_.size() - 1);
        item.iGroupId = group;
        int idx = ListView_InsertItem(list_, &item);
        ListView_SetItemText(list_, idx, kColType, const_cast<LPWSTR>(type.c_str()));
        ListView_SetItemText(list_, idx, kColConnection, const_cast<LPWSTR>(connection.c_str()));
        ListView_SetItemText(list_, idx, kColSerial, const_cast<LPWSTR>(serial.c_str()));
        ListView_SetItemText(list_, idx, kColStatus, const_cast<LPWSTR>(status.c_str()));
        if (!selectKey.empty() && IEquals(key, selectKey)) selectIndex = idx;
    };

    auto groupFor = [&](const DevNodeInfo& d) {
        if (IEquals(d.className, L"Camera")) return (int)kGroupCameras;
        if (IEquals(d.className, L"MEDIA")) return (int)kGroupMedia;
        if (IEquals(d.className, L"Image")) return (int)kGroupImaging;
        std::wstring cls = d.className.empty() ? L"Other devices" : d.className;
        auto it = otherGroups.find(cls);
        if (it == otherGroups.end()) {
            int id = kGroupOtherBase + (int)otherGroups.size();
            addGroup(id, cls);
            it = otherGroups.emplace(cls, id).first;
        }
        return it->second;
    };
    auto addDevices = [&](auto inGroup) {
        for (size_t i = 0; i < inventory_.devices.size(); ++i) {
            const DevNodeInfo& d = inventory_.devices[i];
            if (IEquals(d.className, L"AudioEndpoint")) continue;  // shown as sound endpoints
            if (!all && !IsMediaClass(d.className)) continue;
            if (!filter.empty() && !IContains(d.DisplayName(), filter) && !IContains(d.instanceId, filter) &&
                !IContains(d.className, filter))
                continue;
            int group = groupFor(d);
            if (!inGroup(group)) continue;
            addItem({false, i}, group, d.DisplayName(), TypeLabel(d.className), Connection(d), d.serial,
                    state_.IsTracked(d.instanceId) ? L"Renamed" : L"", d.instanceId);
        }
    };
    auto addEndpoints = [&](bool capture) {
        for (size_t i = 0; i < inventory_.endpoints.size(); ++i) {
            const AudioEndpointInfo& e = inventory_.endpoints[i];
            if (e.capture != capture || (!filter.empty() && !IContains(e.friendlyName, filter))) continue;
            std::wstring status =
                state_.IsTracked(e.id) ? L"Renamed" : e.state != DEVICE_STATE_ACTIVE ? L"Disabled" : L"";
            addItem({true, i}, capture ? kGroupRecording : kGroupPlayback, e.friendlyName,
                    capture ? L"Recording" : L"Playback", L"", L"", status, e.id);
        }
    };
    // Insert in the same order the groups are displayed, so item 0 is the top row.
    addDevices([](int g) { return g == kGroupCameras; });
    addEndpoints(true);
    addEndpoints(false);
    addDevices([](int g) { return g == kGroupMedia; });
    addDevices([](int g) { return g == kGroupImaging; });
    addDevices([](int g) { return g >= kGroupOtherBase; });

    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    if (selectIndex < 0 && ListView_GetItemCount(list_) > 0) selectIndex = 0;
    if (selectIndex >= 0) {
        ListView_SetItemState(list_, selectIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, selectIndex, FALSE);
    }
    OnSelectionChanged();
    UpdateStatus();
}

const MainWindow::Row* MainWindow::SelectedRow() const {
    int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (sel < 0) return nullptr;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    if (!ListView_GetItem(list_, &item) || (size_t)item.lParam >= rows_.size()) return nullptr;
    return &rows_[(size_t)item.lParam];
}

const DevNodeInfo* MainWindow::SelectedDevice() const {
    const Row* r = SelectedRow();
    return r && !r->endpoint ? &inventory_.devices[r->index] : nullptr;
}

const AudioEndpointInfo* MainWindow::SelectedEndpoint() const {
    const Row* r = SelectedRow();
    return r && r->endpoint ? &inventory_.endpoints[r->index] : nullptr;
}

std::wstring MainWindow::SelectedKey() const {
    if (const DevNodeInfo* d = SelectedDevice()) return d->instanceId;
    if (const AudioEndpointInfo* e = SelectedEndpoint()) return e->id;
    return L"";
}

std::wstring MainWindow::DetailsText() const {
    const wchar_t* nl = L"\r\n";
    std::wstring t;
    if (const AudioEndpointInfo* e = SelectedEndpoint()) {
        t += e->friendlyName + nl + nl;
        t += std::wstring(L"Kind: ") + (e->capture ? L"Recording" : L"Playback") + nl;
        t += L"Description: " + e->deviceDesc + nl;
        t += L"Device name: " + e->interfaceName + nl;
        t += L"Endpoint id: " + e->id + nl;
        if (!IsNullOrRootContainer(e->containerId)) {
            for (const auto& d : inventory_.devices)
                if (IEquals(d.containerId, e->containerId) && IEquals(d.className, L"Camera"))
                    t += L"Part of camera: " + d.DisplayName() + nl;
        }
        if (state_.IsTracked(e->id)) t += std::wstring(nl) + L"Renamed by Windira. Restore original puts the old name back." + nl;
        return t;
    }
    const DevNodeInfo* d = SelectedDevice();
    if (!d) return L"";
    t += d->DisplayName() + nl + nl;
    t += L"Type: " + TypeLabel(d->className) + nl;
    if (!d->deviceDesc.empty()) t += L"Driver description: " + d->deviceDesc + nl;
    if (!d->manufacturer.empty()) t += L"Manufacturer: " + d->manufacturer + nl;
    std::wstring conn = Connection(*d);
    if (!conn.empty()) t += L"Connection: " + conn + nl;
    t += L"Serial number: " + (d->serial.empty() ? std::wstring(L"none (Windows tells it apart by its USB port)") : d->serial) + nl;
    t += L"Instance id: " + d->instanceId + nl;
    size_t named = d->NamedInterfaceCount();
    if (named) {
        t += std::wstring(nl) + L"Name apps see (camera / audio apps):" + nl;
        for (const auto& i : d->interfaces)
            if (i.hasFriendlyName) t += L"   " + i.friendlyName + L"  [" + InterfaceCategoryName(i.classGuid) + L"]" + nl;
    }
    if (!IsNullOrRootContainer(d->containerId)) {
        std::wstring same;
        for (const auto& o : inventory_.devices)
            if (&o != d && IEquals(o.containerId, d->containerId) && !IEquals(o.className, L"AudioEndpoint"))
                same += L"   " + o.DisplayName() + L"  [" + TypeLabel(o.className) + L"]" + nl;
        for (const auto& e : inventory_.endpoints)
            if (IEquals(e.containerId, d->containerId)) same += L"   " + e.friendlyName + L"  [Sound]" + nl;
        if (!same.empty()) t += std::wstring(nl) + L"Same physical device:" + nl + same;
    }
    if (state_.IsTracked(d->instanceId))
        t += std::wstring(nl) + L"Renamed by Windira. Restore original puts the old names back." + nl;
    return t;
}

void MainWindow::OnSelectionChanged() {
    if (preview_.Active()) preview_.Stop();
    SetWindowTextW(identify_, L"&Identify");
    preview_.ShowMessage(kIdleHint);
    SetWindowTextW(details_, DetailsText().c_str());
    UpdateButtons();
}

void MainWindow::UpdateButtons() {
    std::wstring key = SelectedKey();
    // Without admin rights these stay enabled and offer to reopen Windira as administrator.
    EnableWindow(rename_, !key.empty());
    EnableWindow(restore_, !key.empty() && state_.IsTracked(key));
    EnableWindow(identify_, !key.empty());
    EnableWindow(reset_, !resetting_ && !ResetTarget().empty());
}

void MainWindow::UpdateStatus(const std::wstring& message) {
    std::wstring text = message;
    if (text.empty()) {
        size_t groups = 0;
        std::vector<std::wstring> seen;
        for (const auto& e : state_.entries())
            if (std::none_of(seen.begin(), seen.end(), [&](const std::wstring& g) { return IEquals(g, e.group); }))
                seen.push_back(e.group), ++groups;
        text = std::to_wstring(ListView_GetItemCount(list_)) + L" devices shown   |   " + std::to_wstring(groups) +
               L" renamed by Windira";
        if (!elevated_) text += L"   |   Not running as administrator - renaming will ask to reopen Windira as administrator";
    }
    SendMessageW(status_, SB_SETTEXTW, 0, (LPARAM)text.c_str());
}

void MainWindow::ReportResults(const std::wstring& action, const std::vector<ChangeResult>& results) {
    std::wstring failures;
    size_t failed = 0;
    for (const auto& r : results) {
        if (r.status) continue;
        ++failed;
        failures += L"• " + r.change.label + L"\n   " + r.status.message + L"\n";
    }
    if (failed) {
        std::wstring msg = action + L" finished with " + std::to_wstring(failed) + L" problem(s):\n\n" + failures;
        MessageBoxW(hwnd_, msg.c_str(), L"Windira", MB_ICONWARNING);
    }
}

void MainWindow::OnRename() {
    if (!EnsureElevated()) return;
    preview_.Stop();
    std::vector<ChangeResult> results;
    std::wstring newName, key;
    bool restart = false;

    if (const DevNodeInfo* d = SelectedDevice()) {
        DeviceRenameRequest req;
        req.inventory = &inventory_;
        req.device = d;
        if (!ShowDeviceRenameDialog(hwnd_, req)) return;
        key = d->instanceId;
        newName = req.name;
        restart = req.restart;
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        results = ApplyChanges(state_, PlanDeviceRename(inventory_, *d, req.name, req.includeRelated));
        SetCursor(old);
    } else if (const AudioEndpointInfo* e = SelectedEndpoint()) {
        EndpointRenameRequest req;
        req.endpoint = e;
        if (!ShowEndpointRenameDialog(hwnd_, req)) return;
        key = e->id;
        newName = req.description + L" (" + req.deviceName + L")";
        bool descChanged = req.description != e->deviceDesc;
        bool devChanged = req.deviceName != e->interfaceName;
        if (!descChanged && !devChanged) return;
        results = ApplyChanges(state_, PlanEndpointRename(*e, descChanged ? &req.description : nullptr,
                                                          devChanged ? &req.deviceName : nullptr));
    } else {
        return;
    }

    ReportResults(L"Renaming", results);
    bool reboot = false;
    if (restart) {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        for (const auto& id : DevicesInChanges(results)) {
            bool r = false;
            if (RestartDevice(id, r) && r) reboot = true;
        }
        SetCursor(old);
    }
    Reload(key);
    bool anyOk = std::any_of(results.begin(), results.end(), [](const ChangeResult& r) { return r.status.ok; });
    if (anyOk) {
        UpdateStatus(L"Renamed to “" + newName + L"”. Apps that are already open may need a restart to show it." +
                     (reboot ? L" Windows asks for a reboot to restart the device." : L""));
    }
}

void MainWindow::OnRestore() {
    std::wstring key = SelectedKey();
    if (key.empty() || !EnsureElevated()) return;
    preview_.Stop();
    auto results = Restore(state_, key);
    ReportResults(L"Restoring", results);
    Reload(key);
    UpdateStatus(L"Original names restored.");
}

void MainWindow::OnIdentify() {
    if (preview_.Active()) {
        preview_.Stop();
        preview_.ShowMessage(kIdleHint);
        SetWindowTextW(identify_, L"&Identify");
        return;
    }
    if (const AudioEndpointInfo* e = SelectedEndpoint()) {
        if (preview_.StartMeter(e->id, e->capture)) SetWindowTextW(identify_, L"Stop &identify");
        return;
    }
    const DevNodeInfo* d = SelectedDevice();
    if (!d) return;
    bool camera = IEquals(d->className, L"Camera") || IEquals(d->className, L"Image");
    if (!camera) {
        // The audio function of a webcam or headset: show its microphone level.
        for (const auto& e : inventory_.endpoints) {
            if (e.capture && !IsNullOrRootContainer(d->containerId) && IEquals(e.containerId, d->containerId)) {
                if (preview_.StartMeter(e.id, true)) SetWindowTextW(identify_, L"Stop &identify");
                return;
            }
        }
    }
    std::vector<std::wstring> links;
    for (const auto& i : d->interfaces) links.push_back(i.symbolicLink);
    preview_.StartCamera(links);
    SetWindowTextW(identify_, L"Stop &identify");
}

void MainWindow::OnToggleTask() {
    if (!EnsureElevated()) {
        Button_SetCheck(autoTask_, taskInstalled_ ? BST_CHECKED : BST_UNCHECKED);
        return;
    }
    bool want = Button_GetCheck(autoTask_) == BST_CHECKED;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    Status s = want ? InstallReapplyTask(DefaultCliPath()) : RemoveReapplyTask();
    SetCursor(old);
    if (!s) {
        MessageBoxW(hwnd_, s.message.c_str(), L"Windira", MB_ICONWARNING);
    }
    taskInstalled_ = IsReapplyTaskInstalled();
    Button_SetCheck(autoTask_, taskInstalled_ ? BST_CHECKED : BST_UNCHECKED);
    UpdateStatus(taskInstalled_ ? L"Your names are now re-applied automatically after driver updates and at startup."
                                : L"Automatic re-apply is off.");
}

struct ResetJob {
    std::wstring selectKey;
    bool retry = false;
    std::vector<std::wstring> closeLog;
    ResetOutcome outcome;
};

std::wstring MainWindow::ResetTarget() const {
    if (const DevNodeInfo* d = SelectedDevice()) return ResetTargetFor(inventory_, *d);
    if (const AudioEndpointInfo* e = SelectedEndpoint()) {
        // An endpoint resets through the physical device it belongs to (e.g. the webcam).
        if (IsNullOrRootContainer(e->containerId)) return L"";
        for (const auto& d : inventory_.devices)
            if (IEquals(d.containerId, e->containerId) && IsMediaClass(d.className) &&
                !IEquals(d.className, L"AudioEndpoint"))
                return ResetTargetFor(inventory_, d);
    }
    return L"";
}

void MainWindow::OnReset() {
    std::wstring target = ResetTarget();
    if (resetting_ || target.empty() || !EnsureElevated()) return;
    const DevNodeInfo* t = inventory_.FindDevice(target);
    std::wstring name = t ? t->DisplayName() : target;
    if (const DevNodeInfo* d = SelectedDevice()) {
        if (!IEquals(d->instanceId, target)) name = d->DisplayName() + L" (" + name + L")";
    }
    std::wstring prompt = L"Reset “" + name +
                          L"”?\n\nIt disconnects for a moment, like unplugging it and plugging it back in. "
                          L"Apps that are using it lose it briefly.";
    if (MessageBoxW(hwnd_, prompt.c_str(), L"Reset device", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    StartReset(target, SelectedKey(), false, {});
}

void MainWindow::StartReset(const std::wstring& target, const std::wstring& selectKey, bool retry,
                            const std::vector<Blocker>& closeFirst) {
    preview_.Stop();  // Windira's own preview must not be what holds the device
    resetting_ = true;
    UpdateButtons();
    UpdateStatus(closeFirst.empty() ? L"Resetting the device…" : L"Closing programs and resetting the device…");
    HWND hwnd = hwnd_;
    // Off the UI thread: Windows waits for every program to answer before it can restart
    // the device, and a hung program makes that take a while.
    std::thread([hwnd, target, selectKey, retry, closeFirst]() {
        auto* job = new ResetJob;
        job->selectKey = selectKey;
        job->retry = retry;
        if (!closeFirst.empty()) {
            job->closeLog = CloseBlockers(closeFirst);
            Sleep(500);
        }
        job->outcome = ResetDevice(target);
        if (!PostMessageW(hwnd, WM_APP_RESET_DONE, 0, (LPARAM)job)) delete job;
    }).detach();
}

void MainWindow::OnResetDone(ResetJob* raw) {
    std::unique_ptr<ResetJob> job(raw);
    resetting_ = false;
    const ResetOutcome& r = job->outcome;
    std::wstring closed;
    for (const auto& line : job->closeLog) closed += L"• " + line + L"\n";

    auto blockerList = [&]() {
        std::wstring list;
        for (const auto& b : r.blockers) {
            list += L"• " + b.DisplayName();
            if (!b.canClose) list += L"\n    can't be closed: " + b.reason;
            list += L"\n";
        }
        return list;
    };

    switch (r.result) {
        case ResetOutcome::Result::Restarted:
            Reload(job->selectKey);
            UpdateStatus(L"Device reset. It should work again in every app.");
            if (!closed.empty()) {
                std::wstring msg = L"The device was reset.\n\n" + closed +
                                   L"\nYou can open the closed programs again now.";
                MessageBoxW(hwnd_, msg.c_str(), L"Reset device", MB_ICONINFORMATION);
            }
            return;

        case ResetOutcome::Result::Blocked: {
            bool anyClosable = std::any_of(r.blockers.begin(), r.blockers.end(), [](const Blocker& b) { return b.canClose; });
            if (!job->retry && anyClosable) {
                std::wstring msg = L"These programs are holding the device and refused to let go:\n\n" + blockerList() +
                                   L"\nClose them and try again? You can open them again afterwards. "
                                   L"Unsaved work in them is lost.";
                if (MessageBoxW(hwnd_, msg.c_str(), L"Reset device", MB_YESNO | MB_ICONWARNING) == IDYES) {
                    StartReset(r.deviceId, job->selectKey, true, r.blockers);
                    return;
                }
                UpdateStatus(L"Reset cancelled.");
            } else {
                std::wstring msg = (job->retry ? closed + L"\nThe device is still held by:\n\n"
                                               : std::wstring(L"The device is held by:\n\n")) +
                                   blockerList() + L"\nUnplug it and plug it back in, or restart Windows to reset it.";
                MessageBoxW(hwnd_, msg.c_str(), L"Reset device", MB_ICONWARNING);
                UpdateStatus(L"The device could not be reset.");
            }
            break;
        }

        case ResetOutcome::Result::RebootRequired: {
            std::wstring msg = closed + (closed.empty() ? L"" : L"\n") +
                               L"Windows could not restart the device, and no program reported holding it - "
                               L"its driver is probably stuck.\n\nUnplug it and plug it back in. If that does not "
                               L"help, restart Windows.";
            MessageBoxW(hwnd_, msg.c_str(), L"Reset device", MB_ICONWARNING);
            UpdateStatus(L"The device could not be reset.");
            break;
        }

        case ResetOutcome::Result::Failed:
            MessageBoxW(hwnd_, r.message.c_str(), L"Reset device", MB_ICONERROR);
            UpdateStatus(L"The device could not be reset.");
            break;
    }
    UpdateButtons();
}

bool MainWindow::EnsureElevated() {
    if (elevated_) return true;
    if (MessageBoxW(hwnd_,
                    L"Changing devices needs administrator rights, and Windira is running without them.\n\n"
                    L"Reopen Windira as administrator now?",
                    L"Windira", MB_YESNO | MB_ICONINFORMATION) != IDYES)
        return false;
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.hwnd = hwnd_;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) DestroyWindow(hwnd_);  // the elevated copy takes over
    return false;
}

void MainWindow::ShowAbout() {
    std::wstring text = std::wstring(L"Windira ") + kAppVersion +
                        L"\n\nGive your devices names you can tell apart: Device Manager, camera apps and "
                        L"sound settings, all at once. No reboot needed.\n\n"
                        L"Free and open source software (MIT License).\n"
                        L"Copyright (c) 2026 opensourcera - github.com/opensourcera/windira\n"
                        L"Contact: open.source@gmail.com\n\n"
                        L"Rename history: " + StateStore::DefaultPath();
    MessageBoxW(hwnd_, text.c_str(), L"About Windira", MB_ICONINFORMATION);
}

}  // namespace windira
