// SPDX-License-Identifier: MIT
#include "State.h"

#include <algorithm>

#include "Json.h"

namespace windira {

namespace {

constexpr int kStateVersion = 1;

Status ReadFileUtf8(const std::wstring& path, std::string& out, bool& missing) {
    missing = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            missing = true;
            return Status::Ok();
        }
        return Status::Fail(L"Cannot open " + path + L": " + Win32ErrorMessage(err));
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = out.empty() || ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(f);
    if (!ok) return Status::Fail(L"Cannot read " + path);
    out.resize(read);
    return Status::Ok();
}

Status WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return Status::Fail(L"Cannot write " + tmp + L": " + Win32ErrorMessage(GetLastError()));
    DWORD written = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || written != data.size()) {
        DeleteFileW(tmp.c_str());
        return Status::Fail(L"Cannot write " + tmp);
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp.c_str());
        return Status::Fail(L"Cannot replace " + path + L": " + Win32ErrorMessage(err));
    }
    return Status::Ok();
}

std::wstring Str(const json::Value& obj, const char* key) {
    const json::Value* v = obj.Find(key);
    return v ? Utf8ToWide(v->AsString()) : std::wstring();
}

}  // namespace

std::wstring NowIso8601() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring StateStore::DefaultPath() {
    return ProgramDataDirectory() + L"\\renames.json";
}

Status StateStore::Load(const std::wstring& path) {
    path_ = path;
    entries_.clear();
    std::string text;
    bool missing = false;
    Status s = ReadFileUtf8(path, text, missing);
    if (!s || missing) return s;

    json::Value root;
    std::string error;
    if (!json::Value::Parse(text, root, &error) || !root.IsObject())
        return Status::Fail(L"State file " + path + L" is damaged: " + Utf8ToWide(error));
    const json::Value* list = root.Find("entries");
    if (!list || !list->IsArray()) return Status::Ok();

    for (const json::Value& item : list->Items()) {
        if (!item.IsObject()) continue;
        RenameEntry e;
        if (!ParseTargetKind(Str(item, "kind"), e.kind)) continue;
        e.key = Str(item, "key");
        if (e.key.empty()) continue;
        e.group = Str(item, "group");
        e.label = Str(item, "label");
        const json::Value* original = item.Find("original");
        e.original.exists = original && !original->IsNull();
        if (e.original.exists) e.original.value = Utf8ToWide(original->AsString());
        e.desired = Str(item, "desired");
        e.updated = Str(item, "updated");
        entries_.push_back(std::move(e));
    }
    return Status::Ok();
}

Status StateStore::Save() const {
    json::Value root = json::Value::MakeObject();
    root.Set("version", kStateVersion);
    root.Set("generator", WideToUtf8(std::wstring(kAppName) + L" " + kAppVersion));
    json::Value list = json::Value::MakeArray();
    for (const RenameEntry& e : entries_) {
        json::Value item = json::Value::MakeObject();
        item.Set("kind", WideToUtf8(TargetKindId(e.kind)));
        item.Set("key", WideToUtf8(e.key));
        item.Set("group", WideToUtf8(e.group));
        item.Set("label", WideToUtf8(e.label));
        // null means "there was no value", so restoring deletes the override.
        item.Set("original", e.original.exists ? json::Value(WideToUtf8(e.original.value)) : json::Value());
        item.Set("desired", WideToUtf8(e.desired));
        item.Set("updated", WideToUtf8(e.updated));
        list.Push(std::move(item));
    }
    root.Set("entries", std::move(list));
    std::wstring path = path_.empty() ? DefaultPath() : path_;
    if (IEquals(path, DefaultPath())) {
        Status s = EnsureSecureDirectory(ProgramDataDirectory());
        if (!s) return s;
    }
    // The file is replaced rather than rewritten, so it always gets the folder's permissions.
    return WriteFileAtomic(path, root.Serialize(2) + "\n");
}

RenameEntry* StateStore::Find(TargetKind kind, const std::wstring& key) {
    for (auto& e : entries_)
        if (e.kind == kind && IEquals(e.key, key)) return &e;
    return nullptr;
}

void StateStore::Remove(TargetKind kind, const std::wstring& key) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const RenameEntry& e) { return e.kind == kind && IEquals(e.key, key); }),
                   entries_.end());
}

bool StateStore::IsTracked(const std::wstring& groupOrKey) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const RenameEntry& e) {
        return IEquals(e.group, groupOrKey) || IEquals(e.key, groupOrKey);
    });
}

}  // namespace windira
