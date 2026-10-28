// SPDX-License-Identifier: MIT
#include "Targets.h"

#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <setupapi.h>

#include "AudioKeys.h"

namespace windira {

namespace {

struct KindInfo {
    TargetKind kind;
    const wchar_t* id;
    const wchar_t* label;
};

const KindInfo kKinds[] = {
    {TargetKind::DeviceName, L"device", L"Device Manager name"},
    {TargetKind::InterfaceName, L"interface", L"App-visible interface name"},
    {TargetKind::EndpointDescription, L"endpoint-description", L"Sound endpoint description"},
    {TargetKind::EndpointDeviceName, L"endpoint-device", L"Sound endpoint device name"},
};

// ---- Device node FriendlyName ------------------------------------------------------------

class DevInfo {
public:
    explicit DevInfo(const std::wstring& instanceId) {
        set_ = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        if (set_ == INVALID_HANDLE_VALUE) return;
        data_.cbSize = sizeof(data_);
        ok_ = SetupDiOpenDeviceInfoW(set_, instanceId.c_str(), nullptr, 0, &data_) != FALSE;
        if (!ok_) error_ = GetLastError();
    }
    ~DevInfo() {
        if (set_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set_);
    }
    bool ok() const { return ok_; }
    DWORD error() const { return error_; }
    HDEVINFO set() const { return set_; }
    SP_DEVINFO_DATA* data() { return &data_; }

private:
    HDEVINFO set_ = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA data_{};
    bool ok_ = false;
    DWORD error_ = 0;
};

Status ReadDeviceName(const std::wstring& instanceId, TargetValue& out) {
    out = {};
    DevInfo dev(instanceId);
    if (!dev.ok()) return Status::Fail(L"Device not found: " + instanceId);
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(dev.set(), dev.data(), SPDRP_FRIENDLYNAME, &type, nullptr, 0, &size);
    DWORD err = GetLastError();
    if (err == ERROR_INVALID_DATA || err == ERROR_FILE_NOT_FOUND) return Status::Ok();  // no FriendlyName
    if (err != ERROR_INSUFFICIENT_BUFFER) return Status::Fail(L"Cannot read device name: " + Win32ErrorMessage(err));
    std::vector<BYTE> buf(size + sizeof(wchar_t) * 2, 0);
    if (!SetupDiGetDeviceRegistryPropertyW(dev.set(), dev.data(), SPDRP_FRIENDLYNAME, &type, buf.data(), size, &size))
        return Status::Fail(L"Cannot read device name: " + Win32ErrorMessage(GetLastError()));
    out.exists = true;
    out.value = reinterpret_cast<const wchar_t*>(buf.data());
    return Status::Ok();
}

Status WriteDeviceName(const std::wstring& instanceId, const TargetValue& v) {
    DevInfo dev(instanceId);
    if (!dev.ok()) return Status::Fail(L"Device not found: " + instanceId);
    BOOL ok;
    if (v.exists) {
        ok = SetupDiSetDeviceRegistryPropertyW(dev.set(), dev.data(), SPDRP_FRIENDLYNAME,
                                               reinterpret_cast<const BYTE*>(v.value.c_str()),
                                               (DWORD)((v.value.size() + 1) * sizeof(wchar_t)));
    } else {
        ok = SetupDiSetDeviceRegistryPropertyW(dev.set(), dev.data(), SPDRP_FRIENDLYNAME, nullptr, 0);
        if (!ok && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_INVALID_DATA)) ok = TRUE;
    }
    if (!ok) {
        DWORD err = GetLastError();
        std::wstring msg = L"Cannot change device name: " + Win32ErrorMessage(err);
        if (err == ERROR_ACCESS_DENIED) msg += L" - run Windira as administrator.";
        return Status::Fail(msg);
    }
    return Status::Ok();
}

// ---- Device interface Device Parameters\FriendlyName ------------------------------------

Status OpenInterfaceKey(const std::wstring& link, REGSAM access, HKEY& key) {
    key = nullptr;
    CONFIGRET cr = CM_Open_Device_Interface_KeyW(link.c_str(), access, RegDisposition_OpenExisting, &key, 0);
    if (cr == CR_SUCCESS) return Status::Ok();
    DWORD err = CM_MapCrToWin32Err(cr, ERROR_GEN_FAILURE);
    std::wstring msg = L"Cannot open interface key: " + Win32ErrorMessage(err);
    if (err == ERROR_ACCESS_DENIED) msg += L" - run Windira as administrator.";
    return Status::Fail(msg);
}

Status ReadInterfaceName(const std::wstring& link, TargetValue& out) {
    out = {};
    HKEY key;
    Status s = OpenInterfaceKey(link, KEY_QUERY_VALUE, key);
    if (!s) return s;
    DWORD size = 0;
    LONG rc = RegGetValueW(key, nullptr, L"FriendlyName", RRF_RT_REG_SZ, nullptr, nullptr, &size);
    if (rc == ERROR_SUCCESS && size) {
        std::wstring value(size / sizeof(wchar_t), L'\0');
        rc = RegGetValueW(key, nullptr, L"FriendlyName", RRF_RT_REG_SZ, nullptr, value.data(), &size);
        if (rc == ERROR_SUCCESS) {
            value.resize(wcslen(value.c_str()));
            out.exists = true;
            out.value = value;
        }
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
        return Status::Fail(L"Cannot read interface name: " + Win32ErrorMessage(rc));
    return Status::Ok();
}

Status WriteInterfaceName(const std::wstring& link, const TargetValue& v) {
    HKEY key;
    Status s = OpenInterfaceKey(link, KEY_SET_VALUE, key);
    if (!s) return s;
    LONG rc;
    if (v.exists) {
        rc = RegSetValueExW(key, L"FriendlyName", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.value.c_str()),
                            (DWORD)((v.value.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, L"FriendlyName");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return Status::Fail(L"Cannot change interface name: " + Win32ErrorMessage(rc));
    return Status::Ok();
}

// ---- Audio endpoint properties -----------------------------------------------------------

// "{0.0.1.00000000}.{guid}" -> registry Properties key path. Flow digit: 0 = render, 1 = capture.
bool EndpointRegistryPath(const std::wstring& id, std::wstring& path) {
    size_t dot = id.find(L"}.{");
    if (id.size() < 8 || id[0] != L'{' || dot == std::wstring::npos) return false;
    std::wstring flow = id[5] == L'1' ? L"Capture" : L"Render";
    path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\" + flow + L"\\" + id.substr(dot + 2) +
           L"\\Properties";
    return true;
}

void EndpointKey(TargetKind kind, const PROPERTYKEY*& pkey, const wchar_t*& regValue) {
    if (kind == TargetKind::EndpointDescription) {
        pkey = &kPKEY_Endpoint_DeviceDesc;
        regValue = kRegValue_Endpoint_DeviceDesc;
    } else {
        pkey = &kPKEY_Endpoint_InterfaceName;
        regValue = kRegValue_Endpoint_InterfaceName;
    }
}

Status OpenEndpoint(const std::wstring& id, IMMDevice** device) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return Status::Fail(L"Cannot open the audio device list: " + HResultMessage(hr));
    hr = enumerator->GetDevice(id.c_str(), device);
    enumerator->Release();
    if (FAILED(hr)) return Status::Fail(L"Audio endpoint not found: " + id);
    return Status::Ok();
}

Status ReadEndpointProperty(TargetKind kind, const std::wstring& id, TargetValue& out) {
    out = {};
    const PROPERTYKEY* pkey;
    const wchar_t* regValue;
    EndpointKey(kind, pkey, regValue);
    ComInit com;
    IMMDevice* device = nullptr;
    Status s = OpenEndpoint(id, &device);
    if (!s) return s;
    IPropertyStore* props = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    device->Release();
    if (FAILED(hr)) return Status::Fail(L"Cannot read audio endpoint properties: " + HResultMessage(hr));
    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = props->GetValue(*pkey, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        out.exists = true;
        out.value = pv.pwszVal;
    }
    PropVariantClear(&pv);
    props->Release();
    return Status::Ok();
}

Status WriteEndpointRegistry(const std::wstring& id, const wchar_t* regValue, const TargetValue& v) {
    std::wstring path;
    if (!EndpointRegistryPath(id, path)) return Status::Fail(L"Unrecognised audio endpoint id: " + id);
    LONG rc;
    if (v.exists) {
        rc = RegSetKeyValueW(HKEY_LOCAL_MACHINE, path.c_str(), regValue, REG_SZ, v.value.c_str(),
                             (DWORD)((v.value.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, path.c_str(), regValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    if (rc != ERROR_SUCCESS) {
        std::wstring msg = L"Cannot change audio endpoint name: " + Win32ErrorMessage(rc);
        if (rc == ERROR_ACCESS_DENIED) msg += L" - run Windira as administrator.";
        return Status::Fail(msg);
    }
    return Status::Ok();
}

Status WriteEndpointProperty(TargetKind kind, const std::wstring& id, const TargetValue& v) {
    const PROPERTYKEY* pkey;
    const wchar_t* regValue;
    EndpointKey(kind, pkey, regValue);
    ComInit com;
    IMMDevice* device = nullptr;
    Status s = OpenEndpoint(id, &device);
    if (!s) return s;

    // Preferred path: the endpoint property store. This is what the Sound control panel
    // uses, and the audio service notifies running apps (IMMNotificationClient) at once.
    IPropertyStore* props = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_WRITE, &props);
    device->Release();
    if (SUCCEEDED(hr)) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (v.exists) {
            pv.vt = VT_LPWSTR;
            pv.pwszVal = const_cast<LPWSTR>(v.value.c_str());  // SetValue copies the value
        }
        hr = props->SetValue(*pkey, pv);
        if (SUCCEEDED(hr)) hr = props->Commit();
        props->Release();
        if (SUCCEEDED(hr)) return Status::Ok();
    }
    // Fallback: write the backing registry value directly.
    return WriteEndpointRegistry(id, regValue, v);
}

}  // namespace

const wchar_t* TargetKindId(TargetKind kind) {
    for (const auto& k : kKinds)
        if (k.kind == kind) return k.id;
    return L"unknown";
}

const wchar_t* TargetKindLabel(TargetKind kind) {
    for (const auto& k : kKinds)
        if (k.kind == kind) return k.label;
    return L"Unknown";
}

bool ParseTargetKind(const std::wstring& id, TargetKind& out) {
    for (const auto& k : kKinds) {
        if (IEquals(id, k.id)) {
            out = k.kind;
            return true;
        }
    }
    return false;
}

bool ReadInterfaceFriendlyName(const std::wstring& link, std::wstring& out) {
    TargetValue v;
    if (!ReadInterfaceName(link, v) || !v.exists) return false;
    out = v.value;
    return true;
}

Status ReadTarget(TargetKind kind, const std::wstring& key, TargetValue& out) {
    switch (kind) {
        case TargetKind::DeviceName: return ReadDeviceName(key, out);
        case TargetKind::InterfaceName: return ReadInterfaceName(key, out);
        case TargetKind::EndpointDescription:
        case TargetKind::EndpointDeviceName: return ReadEndpointProperty(kind, key, out);
    }
    return Status::Fail(L"Unknown target kind");
}

Status WriteTarget(TargetKind kind, const std::wstring& key, const TargetValue& value) {
    switch (kind) {
        case TargetKind::DeviceName: return WriteDeviceName(key, value);
        case TargetKind::InterfaceName: return WriteInterfaceName(key, value);
        case TargetKind::EndpointDescription:
        case TargetKind::EndpointDeviceName: return WriteEndpointProperty(kind, key, value);
    }
    return Status::Fail(L"Unknown target kind");
}

Status RestartDevice(const std::wstring& instanceId, bool& rebootRequired) {
    rebootRequired = false;
    DevInfo dev(instanceId);
    if (!dev.ok()) return Status::Fail(L"Device not found: " + instanceId);

    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = DICS_PROPCHANGE;
    params.Scope = DICS_FLAG_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(dev.set(), dev.data(), &params.ClassInstallHeader, sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, dev.set(), dev.data())) {
        return Status::Fail(L"Cannot restart device: " + Win32ErrorMessage(GetLastError()));
    }
    SP_DEVINSTALL_PARAMS_W install{};
    install.cbSize = sizeof(install);
    if (SetupDiGetDeviceInstallParamsW(dev.set(), dev.data(), &install))
        rebootRequired = (install.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
    return Status::Ok();
}

}  // namespace windira
