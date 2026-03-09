# taskman-int

A small stealth overlay utility (Win32/GDI+) that displays a translucent overlay window with status text and supports a "stealth" (click-through) mode.

This repository contains a single example `main.cpp` that:
- Creates a topmost, layered, tool-window overlay
- Renders text using GDI+ (anti-aliased)
- Registers a hotkey (F2) to toggle between interactive and click-through (stealth) modes

## Features
- Transparent overlay window
- Double-buffered GDI+ rendering
- Hotkey: F2 to toggle stealth (click-through) mode

## Build
You can build from VS Code using the provided task (default build). The task calls `g++` and links `gdiplus`, `gdi32`, and `user32`.

Or build from a developer PowerShell terminal manually:

```powershell
# From the project root
C:/msys64/ucrt64/bin/g++.exe -g main.cpp -o main.exe -lgdiplus -lgdi32 -luser32 -static-libgcc -static-libstdc++
```

Notes:
- This project uses MinGW/MSYS (the tasks are configured to use `C:/msys64/ucrt64/bin/g++.exe`). Adjust the path if you use a different toolchain.
- The code depends on GDI+ which is provided by Windows. The linker flag `-lgdiplus` is required.

## Run
From PowerShell in the project folder:

```powershell
# Run overlay
Start-Process -FilePath .\main.exe
# Or run directly in the foreground
.\main.exe
```

When running, an overlay window should appear on your desktop. Press F2 to toggle between interactive (clickable/draggable) and stealth (click-through) modes. The overlay text updates to reflect the current mode.

## Hotkeys
- F2 — Toggle stealth/click-through mode

## Troubleshooting
- If the build fails with undefined references to GDI+ symbols, ensure the `-lgdiplus` linker flag is present and you're linking with `g++` (not `gcc`) so the C++ runtime is linked correctly.
- If the overlay doesn't appear, try running `main.exe` from a console to look for runtime errors, or run with administrator privileges if you suspect permission issues.

## Next steps / Ideas
- Add configuration (hotkeys, opacity) via a config file or command-line options
- Add a tray icon and a small UI to toggle modes
- Integrate audio capture / STT and a screen-capture / vision pipeline as in the design

---

If you'd like, I can also: add a simple `README` entry describing how to change the hotkey, add a small settings file, or implement the small code robustness fixes in `main.cpp`. Which would you prefer next?