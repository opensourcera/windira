// SPDX-License-Identifier: MIT
#include "RenameDialog.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include "../core/Renamer.h"
#include "resource.h"

namespace windira {

namespace {

constexpr size_t kMaxNameLength = 200;

std::wstring GetText(HWND dlg, int id) {
    HWND ctl = GetDlgItem(dlg, id);
    std::wstring text(GetWindowTextLengthW(ctl) + 1, L'\0');
    GetWindowTextW(ctl, text.data(), (int)text.size());
    text.resize(wcslen(text.c_str()));
    return text;
}

void RefreshChanges(HWND dlg, const DeviceRenameRequest& req) {
    HWND list = GetDlgItem(dlg, IDC_CHANGES);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListBox_ResetContent(list);
    std::wstring name = Trim(GetText(dlg, IDC_NAME));
    bool related = IsDlgButtonChecked(dlg, IDC_RELATED) == BST_CHECKED;
    int widest = 0;
    HDC dc = GetDC(list);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(list, WM_GETFONT, 0, 0));
    for (const Change& c : PlanDeviceRename(*req.inventory, *req.device, name.empty() ? L"…" : name, related)) {
        ListBox_AddString(list, c.label.c_str());
        SIZE size{};
        GetTextExtentPoint32W(dc, c.label.c_str(), (int)c.label.size(), &size);
        widest = std::max(widest, (int)size.cx);
    }
    SelectObject(dc, old);
    ReleaseDC(list, dc);
    ListBox_SetHorizontalExtent(list, widest + 8);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

bool ValidateName(HWND dlg, int id, const std::wstring& name) {
    if (name.empty() || name.size() > kMaxNameLength) {
        EDITBALLOONTIP tip{sizeof(tip)};
        tip.pszTitle = L"Invalid name";
        tip.pszText = name.empty() ? L"Type a name." : L"The name is too long (200 characters max).";
        tip.ttiIcon = TTI_WARNING;
        Edit_ShowBalloonTip(GetDlgItem(dlg, id), &tip);
        SetFocus(GetDlgItem(dlg, id));
        return false;
    }
    return true;
}

INT_PTR CALLBACK DeviceDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* req = reinterpret_cast<DeviceRenameRequest*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            req = reinterpret_cast<DeviceRenameRequest*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)req);
            SetDlgItemTextW(dlg, IDC_CURRENT, req->device->DisplayName().c_str());
            SendDlgItemMessageW(dlg, IDC_NAME, EM_LIMITTEXT, kMaxNameLength, 0);
            SetDlgItemTextW(dlg, IDC_NAME, req->device->DisplayName().c_str());
            CheckDlgButton(dlg, IDC_RELATED, req->includeRelated ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_RESTART, req->restart ? BST_CHECKED : BST_UNCHECKED);
            if (IsNullOrRootContainer(req->device->containerId)) {
                CheckDlgButton(dlg, IDC_RELATED, BST_UNCHECKED);
                EnableWindow(GetDlgItem(dlg, IDC_RELATED), FALSE);
            }
            RefreshChanges(dlg, *req);
            HWND edit = GetDlgItem(dlg, IDC_NAME);
            SetFocus(edit);
            Edit_SetSel(edit, 0, -1);
            return FALSE;  // focus set manually
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_NAME:
                    if (HIWORD(wp) == EN_CHANGE) RefreshChanges(dlg, *req);
                    return TRUE;
                case IDC_RELATED:
                    RefreshChanges(dlg, *req);
                    return TRUE;
                case IDOK: {
                    std::wstring name = Trim(GetText(dlg, IDC_NAME));
                    if (!ValidateName(dlg, IDC_NAME, name)) return TRUE;
                    req->name = name;
                    req->includeRelated = IsDlgButtonChecked(dlg, IDC_RELATED) == BST_CHECKED;
                    req->restart = IsDlgButtonChecked(dlg, IDC_RESTART) == BST_CHECKED;
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

void RefreshResult(HWND dlg) {
    std::wstring result = Trim(GetText(dlg, IDC_DESC)) + L" (" + Trim(GetText(dlg, IDC_DEVNAME)) + L")";
    SetDlgItemTextW(dlg, IDC_RESULT, result.c_str());
}

INT_PTR CALLBACK EndpointDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* req = reinterpret_cast<EndpointRenameRequest*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            req = reinterpret_cast<EndpointRenameRequest*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)req);
            SetDlgItemTextW(dlg, IDC_CURRENT, req->endpoint->friendlyName.c_str());
            SendDlgItemMessageW(dlg, IDC_DESC, EM_LIMITTEXT, kMaxNameLength, 0);
            SendDlgItemMessageW(dlg, IDC_DEVNAME, EM_LIMITTEXT, kMaxNameLength, 0);
            SetDlgItemTextW(dlg, IDC_DESC, req->endpoint->deviceDesc.c_str());
            SetDlgItemTextW(dlg, IDC_DEVNAME, req->endpoint->interfaceName.c_str());
            RefreshResult(dlg);
            HWND edit = GetDlgItem(dlg, IDC_DEVNAME);
            SetFocus(edit);
            Edit_SetSel(edit, 0, -1);
            return FALSE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_DESC:
                case IDC_DEVNAME:
                    if (HIWORD(wp) == EN_CHANGE) RefreshResult(dlg);
                    return TRUE;
                case IDOK: {
                    std::wstring desc = Trim(GetText(dlg, IDC_DESC));
                    std::wstring dev = Trim(GetText(dlg, IDC_DEVNAME));
                    if (!ValidateName(dlg, IDC_DESC, desc) || !ValidateName(dlg, IDC_DEVNAME, dev)) return TRUE;
                    req->description = desc;
                    req->deviceName = dev;
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

}  // namespace

bool ShowDeviceRenameDialog(HWND owner, DeviceRenameRequest& request) {
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RENAME_DEVICE), owner, DeviceDlgProc,
                           (LPARAM)&request) == IDOK;
}

bool ShowEndpointRenameDialog(HWND owner, EndpointRenameRequest& request) {
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RENAME_ENDPOINT), owner, EndpointDlgProc,
                           (LPARAM)&request) == IDOK;
}

}  // namespace windira
