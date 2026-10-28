// SPDX-License-Identifier: MIT
#include "Task.h"

#include "State.h"

namespace windira {

namespace {

std::wstring SchtasksPath() {
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\schtasks.exe";
}

// Runs a console program without a window and captures its combined output.
Status RunHidden(const std::wstring& exe, const std::wstring& args, DWORD& exitCode, std::wstring& output) {
    exitCode = (DWORD)-1;
    output.clear();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return Status::Fail(L"CreatePipe failed");
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        DWORD err = GetLastError();
        CloseHandle(readPipe);
        return Status::Fail(L"Cannot start " + exe + L": " + Win32ErrorMessage(err));
    }
    std::string raw;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) && read) raw.append(buf, read);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // schtasks writes in the console's OEM code page.
    int n = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), nullptr, 0);
    output.resize(n);
    MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), output.data(), n);
    output = Trim(output);
    return Status::Ok();
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::wstring TaskXml(const std::wstring& cliPath) {
    return LR"(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Author>Windira</Author>
    <Description>Re-applies the device names you set in Windira when Windows or a driver update resets them.</Description>
  </RegistrationInfo>
  <Triggers>
    <BootTrigger>
      <Enabled>true</Enabled>
      <Delay>PT30S</Delay>
    </BootTrigger>
    <EventTrigger>
      <Enabled>true</Enabled>
      <Delay>PT5S</Delay>
      <Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="Microsoft-Windows-Kernel-PnP/Configuration"&gt;&lt;Select Path="Microsoft-Windows-Kernel-PnP/Configuration"&gt;*[System[(EventID=400 or EventID=410)]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription>
    </EventTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>S-1-5-18</UserId>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <StartWhenAvailable>true</StartWhenAvailable>
    <ExecutionTimeLimit>PT5M</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>")" + XmlEscape(cliPath) + LR"("</Command>
      <Arguments>reapply --quiet</Arguments>
    </Exec>
  </Actions>
</Task>
)";
}

}  // namespace

std::wstring DefaultCliPath() {
    return ExecutableDirectory() + L"\\windira-cli.exe";
}

bool IsReapplyTaskInstalled() {
    DWORD code;
    std::wstring out;
    if (!RunHidden(SchtasksPath(), std::wstring(L"/Query /TN \"") + kTaskName + L"\"", code, out)) return false;
    return code == 0;
}

Status InstallReapplyTask(const std::wstring& sourceCliPath) {
    if (GetFileAttributesW(sourceCliPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return Status::Fail(L"windira-cli.exe was not found at " + sourceCliPath);

    // The task runs as SYSTEM, so it must never run an exe a normal user could replace
    // (e.g. one in Downloads). Run a copy from an admin-only folder under Program Files.
    std::wstring installDir = ProgramFilesDirectory();
    Status s = EnsureSecureDirectory(installDir);
    if (!s) return s;
    std::wstring cliPath = installDir + L"\\windira-cli.exe";
    if (!IEquals(sourceCliPath, cliPath) && !CopyFileW(sourceCliPath.c_str(), cliPath.c_str(), FALSE))
        return Status::Fail(L"Cannot copy windira-cli.exe to " + installDir + L": " + Win32ErrorMessage(GetLastError()));

    // Same for the rename history the task applies: rewrite it inside the locked-down folder.
    s = EnsureSecureDirectory(ProgramDataDirectory());
    if (!s) return s;
    StateStore state;
    if (state.Load() && !state.entries().empty()) {
        s = state.Save();
        if (!s) return s;
    }

    // Written to the admin-only folder, not %TEMP%, so nothing unelevated can swap it
    // between writing and registering.
    std::wstring xmlPath = ProgramDataDirectory() + L"\\task.xml";
    std::wstring xml = TaskXml(cliPath);
    HANDLE f = CreateFileW(xmlPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return Status::Fail(L"Cannot write " + xmlPath);
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    WriteFile(f, xml.data(), (DWORD)(xml.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);

    DWORD code;
    std::wstring out;
    s = RunHidden(SchtasksPath(), std::wstring(L"/Create /TN \"") + kTaskName + L"\" /XML \"" + xmlPath + L"\" /F",
                  code, out);
    DeleteFileW(xmlPath.c_str());
    if (!s) return s;
    if (code != 0) return Status::Fail(L"Task Scheduler refused the task: " + out);
    return Status::Ok();
}

Status RemoveReapplyTask() {
    DWORD code;
    std::wstring out;
    Status s = RunHidden(SchtasksPath(), std::wstring(L"/Delete /TN \"") + kTaskName + L"\" /F", code, out);
    if (!s) return s;
    if (code != 0 && IsReapplyTaskInstalled()) return Status::Fail(L"Cannot remove the task: " + out);
    // Remove the copy the task ran from, unless that is the program running right now.
    std::wstring copy = ProgramFilesDirectory() + L"\\windira-cli.exe";
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!IEquals(self, copy) && DeleteFileW(copy.c_str())) RemoveDirectoryW(ProgramFilesDirectory().c_str());
    return Status::Ok();
}

}  // namespace windira
