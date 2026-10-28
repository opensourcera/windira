# Windira: Easily Rename Devices in Windows

Have two of the same webcam, microphone or headset? Windows shows them with identical names,
and recent versions of Windows give you no real way to rename them. **Windira fixes that.**

**Give your devices names you can tell apart.** Windira renames cameras, microphones,
speakers and other devices on Windows 10 and 11, everywhere Windows shows the name:
Device Manager, camera apps (Zoom, Teams, OBS, Discord, browsers) and Sound settings.
There's no reboot, and the original names come back with one click.

Free, open source (MIT), a single portable `.exe` of about 400 KB, with no runtime to install.

## Download

**[⬇ Download Windira 1.0b (Windows 10/11, 64-bit)](https://github.com/opensourcera/windira/releases/latest)**

Unzip it anywhere and run `windira.exe`. There's nothing to install.

### "Windows protected your PC" or an antivirus warning?

You may see a warning when you first run Windira. **This is expected, and it is a false alarm.**

- Windira is free software and its `.exe` is **not code-signed**. Code-signing certificates cost
  money every year. Windows SmartScreen warns about unsigned programs it hasn't seen often, even
  harmless ones.
- Some antivirus tools are also cautious with any program that changes device settings and asks
  for administrator rights, which renaming devices requires.

To run it anyway, click **More info** and then **Run anyway** on the SmartScreen screen.

**Prefer to be 100% sure?** All of the source code is in this repository, so you can read it and
[build the `.exe` yourself](#building) in a few minutes with free Microsoft tools. Your own build
is exactly what you compiled, with no warnings to trust. Each release also lists **SHA-256
checksums** so you can verify that your download matches the published files.

## The problem

Plug in two identical webcams and every app shows two entries called `HD Pro Webcam C920`.
Renaming one in Device Manager doesn't help much. Camera apps don't read that name. They read
a *different* name stored on each of the device's **interfaces**, and the camera's microphone
has a third name in the audio system:

| Where you see it | Where Windows keeps it |
|---|---|
| Device Manager, Settings | Device node `FriendlyName` |
| Zoom, Teams, OBS, browsers (DirectShow / Media Foundation) | `Device Parameters\FriendlyName` of each device interface (*Capture*, *Video*, *Video camera*) |
| Sound settings, every app's microphone list | Audio endpoint properties: `Microphone (HD Pro Webcam C920)` = *description* (*device name*) |

Windira changes all of them together.

## Features

- **Identify before you rename.** *Identify* shows a live picture from the selected camera or a
  live level meter for a microphone, so you know which physical device is which.
  The list also shows each device's USB port and serial number.
- **One rename, every place.** Renaming a webcam also renames its microphone and audio function.
  You can turn this off per rename.
- **Instant.** No reboot. Apps that are already open pick up the new name after they restart.
  Sound settings update immediately.
- **Fully reversible.** The original value of everything Windira touches is kept in
  `%ProgramData%\Windira\renames.json`. *Restore original* puts it back.
- **Names that stay.** Driver updates and Windows Update can reset names. Turn on
  *Keep my names after driver updates* and a small scheduled task re-applies them at startup and
  whenever Windows installs a driver. Nothing stays running in the background.
- **Reset a stuck device without rebooting.** *Reset device* restarts a device as if you unplugged
  it. If a program refuses to let go of it (camera tools such as Logi Tune or iVCam often do),
  Windira names that program and offers to close it and retry. A reboot is only suggested when the
  driver itself is stuck.
- **Command line** for scripting and deployment: `windira-cli`.

## Supported Windows versions

Current release: **Windira 1.0b** (beta).

| Windows | Supported? | Notes |
|---|---|---|
| **Windows 11**, all versions (21H2 → 25H2) | ✅ Yes | Main target. Tested on Windows 11 25H2. |
| **Windows 10**, version 1607 (Anniversary Update) or newer | ✅ Yes | Includes 21H2 / 22H2 and the LTSC editions 2016, 2019 and 2021. |
| Windows 10 before 1607 (1507, 1511) | ❌ No | Missing the per-monitor display scaling APIs Windira uses. |
| Windows 8.1, 8, 7 and older | ❌ No | |
| Windows Server 2016 / 2019 / 2022 / 2025 | ⚠️ Should work | Same core as Windows 10/11, but not tested yet. Feedback welcome. |

**Also needed:**

- **A 64-bit PC (x64).** That covers almost every Windows PC from the last decade. Windows on ARM
  is not tested yet.
- **Administrator rights** to rename, restore or reset devices. Browsing devices and the live
  preview work without them.
- **Nothing to install.** No .NET, no Visual C++ runtime: just run the `.exe`.

*Why only Windows 10 and later?* Older versions keep device names in different places and lack
the modern camera and audio APIs. Windows 7 and 8.1 are also no longer supported by Microsoft.

## Usage

Run `windira.exe`. It asks for administrator rights, because device names live in
machine-wide settings.

1. Select a device. Click **Identify** to see or hear it.
2. Click **Rename…** (or press F2), type a name and confirm.
3. Restart any app that was already open.

### Command line

```text
windira-cli list                                  cameras, media and audio devices
windira-cli list --all --json                     every device, as JSON
windira-cli show "HD Pro Webcam C920"             every name Windows stores for a device
windira-cli rename "HD Pro Webcam C920" "Left camera"
windira-cli rename USB\VID_046D&PID_08E5&MI_00\8&285CC37B&0&0000 "Left camera" --what-if
windira-cli rename-endpoint "Microphone (Left camera)" --description "Desk mic"
windira-cli restore "Left camera"                 put the original names back
windira-cli restore --all
windira-cli reset "Left camera"                   restart it like a replug; names what blocks it
windira-cli reset "Left camera" --close-blockers  ...and close those programs, then retry
windira-cli history
windira-cli task install | remove | status        automatic re-apply after driver updates
```

A device can be given as its instance id, a unique part of it, or its current name.
Changing names needs an elevated terminal.

## Good to know

- **Identical devices without a serial number** are told apart by Windows *by the USB port they
  are plugged into*. Move such a camera to another port and Windows sees a new device with the
  default name. Keep each one on its own port, or rename it again. Devices with a serial number
  (shown in the list) keep their name on any port.
- **Apps cache device lists.** Restart Zoom, Teams, OBS, etc. after renaming.
- **Driver reinstall / Windows Update** can put the original name back. Turn on the automatic
  re-apply option, or run `windira-cli reapply`.
- Monitor names (from the monitor's EDID) are not supported yet.

## Uninstall

1. In Windira, untick *Keep my names after driver updates*, or run `windira-cli task remove`.
2. Optionally restore every name: `windira-cli restore --all`.
3. Delete `windira.exe`, `windira-cli.exe` and `%ProgramData%\Windira`.

## Building

You need Visual Studio 2019 or 2022 with the *Desktop development with C++* workload.
Its bundled CMake and Ninja are enough.

```bat
build.cmd            :: Release build -> build\Release\bin\
build.cmd Debug
build\Release\bin\windira-tests.exe
```

Or open the folder in Visual Studio (CMake project).

## How it works

| Code | Purpose |
|---|---|
| `src/core/Devices.cpp` | Enumerates device nodes (SetupAPI), their interfaces (CfgMgr32) and audio endpoints (MMDevice API) |
| `src/core/Targets.cpp` | Reads and writes each name location |
| `src/core/Renamer.cpp` | Works out what to change for a rename, applies it, restores it, re-applies it |
| `src/core/State.cpp` | The rename history (`renames.json`) |
| `src/core/Reset.cpp` | Device reset: restarts the device and reads Kernel-PnP event 225 to name the program that blocked it |
| `src/core/Task.cpp` | The optional Task Scheduler task (`\Windira\Reapply device names`) |
| `src/app/` | Win32 GUI, including the Media Foundation camera preview |
| `src/cli/` | `windira-cli` |

**Security:** the re-apply task runs as SYSTEM. To make sure a normal user cannot change what it
runs or applies, Windira copies `windira-cli.exe` to `%ProgramFiles%\Windira` and keeps
`%ProgramData%\Windira` writable only by administrators and SYSTEM.

## Contributing

Bug reports with the output of `windira-cli show "<device>"` are very welcome. That output shows
every name Windows keeps for a device. Pull requests: keep the code dependency-free Win32/C++17,
and run `windira-tests.exe` before you push.

## License

MIT © 2026 [opensourcera](https://github.com/opensourcera). See [LICENSE](LICENSE).

Contact: open.source@gmail.com
