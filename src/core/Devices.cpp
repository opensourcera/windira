// SPDX-License-Identifier: MIT
#include "Devices.h"

#include <initguid.h>

#include <cfgmgr32.h>
#include <devpkey.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <setupapi.h>

#include <algorithm>
#include <unordered_map>

#include "AudioKeys.h"
#include "Targets.h"

namespace windira {

namespace {

const wchar_t* const kMediaClasses[] = {L"Camera", L"Image", L"MEDIA", L"AudioEndpoint"};

bool GetRegistryString(HDEVINFO set, SP_DEVINFO_DATA* data, DWORD prop, std::wstring& out) {
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, data, prop, &type, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
    std::vector<BYTE> buf(size + 2 * sizeof(wchar_t), 0);
    if (!SetupDiGetDeviceRegistryPropertyW(set, data, prop, &type, buf.data(), size, &size)) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) return false;
    out = reinterpret_cast<const wchar_t*>(buf.data());  // first string of a MULTI_SZ
    return true;
}

bool GetDeviceString(HDEVINFO set, SP_DEVINFO_DATA* data, const DEVPROPKEY& key, std::wstring& out) {
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    SetupDiGetDevicePropertyW(set, data, &key, &type, nullptr, 0, &size, 0);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
    std::vector<BYTE> buf(size + 2 * sizeof(wchar_t), 0);
    if (!SetupDiGetDevicePropertyW(set, data, &key, &type, buf.data(), size, &size, 0)) return false;
    if (type == DEVPROP_TYPE_STRING || type == DEVPROP_TYPE_STRING_LIST) {
        out = reinterpret_cast<const wchar_t*>(buf.data());
        return true;
    }
    if (type == DEVPROP_TYPE_GUID && size >= sizeof(GUID)) {
        out = GuidToString(*reinterpret_cast<const GUID*>(buf.data()));
        return true;
    }
    return false;
}

std::wstring LastSegment(const std::wstring& instanceId) {
    size_t slash = instanceId.find_last_of(L'\\');
    return slash == std::wstring::npos ? instanceId : instanceId.substr(slash + 1);
}

std::wstring Upper(std::wstring s) {
    CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

// Reads every device interface class registered on the system so we can attach
// interfaces to their device nodes in a single pass.
std::vector<GUID> InterfaceClassGuids() {
    std::vector<GUID> guids;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses", 0,
                      KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
        return guids;
    wchar_t name[128];
    for (DWORD i = 0;; ++i) {
        DWORD len = ARRAYSIZE(name);
        LONG rc = RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        GUID g;
        if (StringToGuid(name, g)) guids.push_back(g);
    }
    RegCloseKey(key);
    return guids;
}

std::vector<std::wstring> PresentInterfaces(const GUID& cls) {
    std::vector<std::wstring> links;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID*>(&cls), nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
            len <= 1)
            return links;
        std::vector<wchar_t> buf(len);
        CONFIGRET cr = CM_Get_Device_Interface_ListW(const_cast<GUID*>(&cls), nullptr, buf.data(), len,
                                                     CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_BUFFER_SMALL) continue;  // an interface arrived between the two calls
        if (cr != CR_SUCCESS) return links;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) links.emplace_back(p);
        return links;
    }
    return links;
}

bool InterfaceInstanceId(const std::wstring& link, std::wstring& out) {
    DEVPROPTYPE type = 0;
    wchar_t buf[MAX_DEVICE_ID_LEN + 1] = {};
    ULONG size = sizeof(buf);
    if (CM_Get_Device_Interface_PropertyW(link.c_str(), &DEVPKEY_Device_InstanceId, &type, (PBYTE)buf, &size, 0) !=
            CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return false;
    out = buf;
    return true;
}

}  // namespace

size_t DevNodeInfo::NamedInterfaceCount() const {
    return (size_t)std::count_if(interfaces.begin(), interfaces.end(),
                                 [](const DeviceInterfaceInfo& i) { return i.hasFriendlyName; });
}

bool IsMediaClass(const std::wstring& className) {
    for (const wchar_t* c : kMediaClasses)
        if (IEquals(className, c)) return true;
    return false;
}

Status EnumerateDevNodes(std::vector<DevNodeInfo>& out) {
    out.clear();
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return Status::Fail(L"SetupDiGetClassDevs failed: " + Win32ErrorMessage(GetLastError()));

    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA data{sizeof(data)};
        if (!SetupDiEnumDeviceInfo(set, i, &data)) break;

        DevNodeInfo dev;
        wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &data, id, ARRAYSIZE(id), nullptr)) continue;
        dev.instanceId = id;
        dev.hasFriendlyName = GetRegistryString(set, &data, SPDRP_FRIENDLYNAME, dev.friendlyName);
        GetRegistryString(set, &data, SPDRP_DEVICEDESC, dev.deviceDesc);
        GetRegistryString(set, &data, SPDRP_CLASS, dev.className);
        GetRegistryString(set, &data, SPDRP_MFG, dev.manufacturer);
        GetRegistryString(set, &data, SPDRP_LOCATION_INFORMATION, dev.locationInfo);
        GetRegistryString(set, &data, SPDRP_HARDWAREID, dev.hardwareId);
        GetDeviceString(set, &data, DEVPKEY_Device_LocationPaths, dev.locationPath);
        GetDeviceString(set, &data, DEVPKEY_Device_ContainerId, dev.containerId);
        GetDeviceString(set, &data, DEVPKEY_Device_Parent, dev.parentId);
        dev.classGuid = GuidToString(data.ClassGuid);
        out.push_back(std::move(dev));
    }
    SetupDiDestroyDeviceInfoList(set);

    std::unordered_map<std::wstring, size_t> byId;
    for (size_t i = 0; i < out.size(); ++i) byId[Upper(out[i].instanceId)] = i;

    // USB serial numbers: a USB device that reports a serial gets it as the last part
    // of its instance id; ids generated by Windows (port based) always contain '&'.
    // Functions of a composite device (&MI_xx) inherit the serial of their parent.
    for (auto& dev : out) {
        if (Upper(dev.instanceId).rfind(L"USB\\", 0) != 0) continue;  // e.g. Bluetooth children of a USB radio
        const DevNodeInfo* cur = &dev;
        for (int depth = 0; cur && depth < 2; ++depth) {
            std::wstring upper = Upper(cur->instanceId);
            if (upper.rfind(L"USB\\", 0) == 0 && upper.find(L"&MI_") == std::wstring::npos) {
                std::wstring last = LastSegment(cur->instanceId);
                if (last.find(L'&') == std::wstring::npos) dev.serial = last;
                break;
            }
            auto it = byId.find(Upper(cur->parentId));
            cur = it == byId.end() ? nullptr : &out[it->second];
        }
    }

    // Attach device interfaces.
    for (const GUID& cls : InterfaceClassGuids()) {
        std::wstring clsText = GuidToString(cls);
        for (const std::wstring& link : PresentInterfaces(cls)) {
            std::wstring owner;
            if (!InterfaceInstanceId(link, owner)) continue;
            auto it = byId.find(Upper(owner));
            if (it == byId.end()) continue;
            DeviceInterfaceInfo info;
            info.symbolicLink = link;
            info.classGuid = clsText;
            info.hasFriendlyName = ReadInterfaceFriendlyName(link, info.friendlyName);
            out[it->second].interfaces.push_back(std::move(info));
        }
    }
    return Status::Ok();
}

Status EnumerateAudioEndpoints(std::vector<AudioEndpointInfo>& out) {
    out.clear();
    ComInit com;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return Status::Fail(L"Cannot open the audio device list: " + HResultMessage(hr));

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED, &collection);
    if (SUCCEEDED(hr)) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;
            AudioEndpointInfo ep;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id))) {
                ep.id = id;
                CoTaskMemFree(id);
            }
            device->GetState(&ep.state);
            IMMEndpoint* endpoint = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&endpoint)))) {
                EDataFlow flow = eRender;
                endpoint->GetDataFlow(&flow);
                ep.capture = flow == eCapture;
                endpoint->Release();
            }
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                auto readString = [&](const PROPERTYKEY& key, std::wstring& dst) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) dst = pv.pwszVal;
                    PropVariantClear(&pv);
                };
                readString(PKEY_Device_FriendlyName, ep.friendlyName);
                readString(PKEY_Device_DeviceDesc, ep.deviceDesc);
                readString(kPKEY_Endpoint_InterfaceName, ep.interfaceName);
                if (ep.interfaceName.empty()) readString(PKEY_DeviceInterface_FriendlyName, ep.interfaceName);
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(kPKEY_Endpoint_ContainerId, &pv)) && pv.vt == VT_CLSID && pv.puuid)
                    ep.containerId = GuidToString(*pv.puuid);
                PropVariantClear(&pv);
                props->Release();
            }
            device->Release();
            if (!ep.id.empty()) out.push_back(std::move(ep));
        }
        collection->Release();
    }
    enumerator->Release();
    if (FAILED(hr)) return Status::Fail(L"Cannot list audio endpoints: " + HResultMessage(hr));
    return Status::Ok();
}

Status Inventory::Load() {
    Status s = EnumerateDevNodes(devices);
    if (!s) return s;
    std::sort(devices.begin(), devices.end(), [](const DevNodeInfo& a, const DevNodeInfo& b) {
        int c = CompareStringOrdinal(a.className.c_str(), -1, b.className.c_str(), -1, TRUE);
        if (c != CSTR_EQUAL) return c == CSTR_LESS_THAN;
        return CompareStringOrdinal(a.DisplayName().c_str(), -1, b.DisplayName().c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    // Audio failures are not fatal: the device list is still useful without them.
    EnumerateAudioEndpoints(endpoints);
    std::sort(endpoints.begin(), endpoints.end(), [](const AudioEndpointInfo& a, const AudioEndpointInfo& b) {
        if (a.capture != b.capture) return a.capture;
        return CompareStringOrdinal(a.friendlyName.c_str(), -1, b.friendlyName.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return Status::Ok();
}

const DevNodeInfo* Inventory::FindDevice(const std::wstring& instanceId) const {
    for (const auto& d : devices)
        if (IEquals(d.instanceId, instanceId)) return &d;
    return nullptr;
}

const AudioEndpointInfo* Inventory::FindEndpoint(const std::wstring& id) const {
    for (const auto& e : endpoints)
        if (IEquals(e.id, id)) return &e;
    return nullptr;
}

}  // namespace windira
