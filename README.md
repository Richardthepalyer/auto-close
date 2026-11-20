# Hotkey Closer (C++ / Win32)

Close your open apps with one global hotkey. Windows 10/11, single standalone `.exe`, no dependencies.

## Build (recommended: MSVC + CMake)
1. Install **Visual Studio Build Tools** (workload *Desktop development with C++*) and **CMake**.
2. Open *x64 Native Tools Command Prompt* (or any terminal) in this folder and run:
   ```
   build.bat
   ```
   or manually:
   ```
   cmake -S . -B build -A x64
   cmake --build build --config Release
   ```
3. Your app: `build\Release\HotkeyCloser.exe` (statically linked, copy it anywhere).

### Build with MinGW-w64 instead
```
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
Or without CMake (from an MSYS2 UCRT64 shell):
```
windres app.rc -O coff -o app.res
g++ -std=c++17 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ main.cpp app.res -o HotkeyCloser.exe -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -ldwmapi -luxtheme -lversion
```

## Run / use
* Double-click `HotkeyCloser.exe` -> the settings window opens.
* **Record hotkey** -> press e.g. `Ctrl+Alt+Q`. Needs at least one modifier. You get a red warning if the combo is invalid or already taken (the old hotkey stays active).
* Closing the window (X) hides it to the tray; the hotkey keeps working. Tray: *Open settings / Close apps now / Quit*.
* **Close mode**
  * *Close all apps*: every visible app window's process, except protected apps + the built-in critical list.
  * *Close only selected apps*: tick apps in the picker. Use the box under the list to add `chrome.exe` etc. by name (remembered even when not running). *Remove entry* forgets a manual one.
* **Closing sequence**: sends a normal close request first (apps can ask "Save changes?"), waits the timeout (default 5 s), then force-kills leftovers if *Force-close* is on. With force off, apps that don't exit are simply left running and reported.
* Everything saves automatically to `%APPDATA%\HotkeyCloser\config.json`.
* *Start with Windows* writes `HKCU\...\Run` with `--tray`, so it starts hidden in the tray.

## Good to know
* **Elevated apps** (run as administrator) can't be closed by a normal-privilege process. Run Hotkey Closer as administrator to close those too (don't combine with autostart unless you create a scheduled task).
* **Store/UWP apps** (Calculator, Settings, ...) are hosted by `ApplicationFrameHost.exe`, which is deliberately protected, so they are not closed.
* Apps that minimise to tray on close (Discord, Steam...) only exit if force-close is on.
* Using a hotkey with the **Win** key can still open the Start menu after recording, since Windows sees Win pressed alone; prefer Ctrl/Alt/Shift combos.
* If the force-close timeout is too short for slow "Save changes?" prompts, raise it or turn force-close off.

## Tweaking the code (all in `main.cpp`)
| Want to change | Look for |
|---|---|
| Built-in never-close list | `kCritical[]` (section 1) |
| Default timeout / max timeout | `DEFAULT_TIMEOUT_SEC`, `MAX_TIMEOUT_SEC` |
| What counts as an "app window" | `IsAppWindow()` (section 4) |
| Close-all / selected selection logic | `BuildTargets()` |
| Graceful -> force sequence | `CloseWorker()` (section 5) |
| Colours | `ColBg()`, `ColCtrl()`, `ColText()` (section 8) |
| Layout | `CreateUI()` (design units at 96 DPI) |
| Window/tray icon | `MakeAppIcon()` (or load your own .ico with `LoadImage`) |
