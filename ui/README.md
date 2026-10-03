# FSRScale UI

Lossless Scaling–style launcher for **FSRScale**.

## Quick run (script)

```powershell
py -3 scale_ui.py
```

By default the UI looks for **`FSRScale.exe` in the same folder** as the UI.

## Build standalone .exe

```powershell
.\build.bat
```

Creates a venv (`.venv`), installs PyInstaller, and writes:

```
build\FSRScaleUI.exe
build\profiles\Default.json
```

Copy `FSRScale.exe` next to `build\FSRScaleUI.exe` (or set the path in the UI).

## Profiles

Stored as JSON under `profiles/` (next to the exe when frozen).
