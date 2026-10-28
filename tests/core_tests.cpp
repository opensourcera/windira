// SPDX-License-Identifier: MIT
// Unit tests for the parts of Windira that do not touch real devices.
// Run: build\Release\bin\windira-tests.exe  (or "ctest" in the build directory)
#include <cstdio>
#include <string>

#include "Common.h"
#include "Json.h"
#include "State.h"
#include "Targets.h"

using namespace windira;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                    \
    } while (0)

static void TestStrings() {
    CHECK(IEquals(L"USB\\VID_046D", L"usb\\vid_046d"));
    CHECK(!IEquals(L"abc", L"abd"));
    CHECK(IContains(L"HD Pro Webcam C920", L"webcam"));
    CHECK(Trim(L"  Left Cam \t") == L"Left Cam");
    CHECK(Utf8ToWide(WideToUtf8(L"Caméra gauche → 1")) == L"Caméra gauche → 1");
    GUID g;
    CHECK(StringToGuid(L"e5323777-f976-4f5b-9b55-b94699c46e44", g));
    CHECK(IEquals(GuidToString(g), L"{E5323777-F976-4F5B-9B55-B94699C46E44}"));
    CHECK(IsNullOrRootContainer(L"{00000000-0000-0000-ffff-ffffffffffff}"));
    CHECK(IsNullOrRootContainer(L""));
    CHECK(!IsNullOrRootContainer(L"{5818EBF1-9E77-5545-9BD6-4524A2333909}"));
}

static void TestJson() {
    json::Value root = json::Value::MakeObject();
    root.Set("name", "Left \"Cam\"\n\\ é");
    root.Set("count", 3);
    root.Set("none", json::Value());
    json::Value arr = json::Value::MakeArray();
    arr.Push(true);
    arr.Push(1.5);
    root.Set("list", arr);

    json::Value parsed;
    std::string err;
    CHECK(json::Value::Parse(root.Serialize(), parsed, &err));
    CHECK(parsed.Find("name")->AsString() == "Left \"Cam\"\n\\ é");
    CHECK(parsed.Find("count")->AsNumber() == 3);
    CHECK(parsed.Find("none")->IsNull());
    CHECK(parsed.Find("list")->Items().size() == 2);
    CHECK(parsed.Find("list")->Items()[1].AsNumber() == 1.5);

    CHECK(json::Value::Parse("{\"a\":\"\\u00e9\\ud83d\\ude00\"}", parsed));
    CHECK(parsed.Find("a")->AsString() == "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(json::Value::Parse("\xEF\xBB\xBF{}", parsed));  // BOM
    CHECK(!json::Value::Parse("{\"a\":1,}", parsed));
    CHECK(!json::Value::Parse("[1 2]", parsed));
    CHECK(!json::Value::Parse("{} x", parsed));
}

static void TestTargetKinds() {
    for (TargetKind k : {TargetKind::DeviceName, TargetKind::InterfaceName, TargetKind::EndpointDescription,
                         TargetKind::EndpointDeviceName}) {
        TargetKind parsed;
        CHECK(ParseTargetKind(TargetKindId(k), parsed) && parsed == k);
    }
    TargetKind dummy;
    CHECK(!ParseTargetKind(L"bogus", dummy));
}

static void TestStateRoundTrip() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"windira-test-state.json";
    DeleteFileW(path.c_str());

    StateStore state;
    CHECK(state.Load(path).ok);  // missing file is fine
    CHECK(state.entries().empty());

    RenameEntry a;
    a.kind = TargetKind::DeviceName;
    a.key = L"USB\\VID_046D&PID_08E5&MI_00\\8&285CC37B&0&0000";
    a.group = a.key;
    a.label = L"Device Manager: HD Pro Webcam C920 [Camera]";
    a.original = {false, L""};
    a.desired = L"Left Cam é";
    a.updated = NowIso8601();
    RenameEntry b = a;
    b.kind = TargetKind::EndpointDeviceName;
    b.key = L"{0.0.1.00000000}.{4cfd0f82-7f2c-4678-b632-650386adf3ad}";
    b.original = {true, L"HD Pro Webcam C920"};
    state.entries().push_back(a);
    state.entries().push_back(b);
    CHECK(state.Save().ok);

    StateStore loaded;
    CHECK(loaded.Load(path).ok);
    CHECK(loaded.entries().size() == 2);
    RenameEntry* la = loaded.Find(TargetKind::DeviceName, L"usb\\vid_046d&pid_08e5&mi_00\\8&285cc37b&0&0000");
    CHECK(la != nullptr);
    if (la) {
        CHECK(!la->original.exists);
        CHECK(la->desired == L"Left Cam é");
    }
    RenameEntry* lb = loaded.Find(TargetKind::EndpointDeviceName, b.key);
    CHECK(lb && lb->original.exists && lb->original.value == L"HD Pro Webcam C920");
    CHECK(loaded.IsTracked(a.key));
    CHECK(loaded.IsTracked(b.key));
    CHECK(!loaded.IsTracked(L"USB\\VID_0000"));

    loaded.Remove(TargetKind::DeviceName, a.key);
    CHECK(loaded.entries().size() == 1);
    CHECK(loaded.IsTracked(a.key));  // still the group of the endpoint entry

    // A damaged file is reported rather than silently discarded.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written;
    WriteFile(f, "{ not json", 10, &written, nullptr);
    CloseHandle(f);
    StateStore broken;
    CHECK(!broken.Load(path).ok);
    DeleteFileW(path.c_str());
}

int main() {
    TestStrings();
    TestJson();
    TestTargetKinds();
    TestStateRoundTrip();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
