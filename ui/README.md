# NRLive UI

Lossless Scaling–style launcher for **NRLive**.

## Quick run (script)

```powershell
py -3 scale_ui.py
```

By default the UI looks for **`NRLive.exe` in the same folder** as the UI.

## Build standalone .exe

```powershell
.\build.bat
```

Creates a venv (`.venv`), installs PyInstaller, and writes:

```
build\NRLiveUI.exe
build\profiles\Default.json
```

Copy `NRLive.exe` next to `build\NRLiveUI.exe` (or set the path in the UI).

## Profiles

Stored as JSON under `profiles/` (next to the exe when frozen).
