@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VENV=%~dp0.venv"
set "BUILDDIR=%~dp0build"
set "DISTDIR=%BUILDDIR%"

echo === FSRScale UI build ===
echo.

where py >nul 2>&1
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  where python >nul 2>&1
  if %ERRORLEVEL%==0 (
    set "PY=python"
  ) else (
    echo ERROR: Python 3 not found. Install from https://www.python.org/ and enable "Add to PATH".
    exit /b 1
  )
)

echo [1/4] Creating virtual environment...
if not exist "%VENV%\Scripts\python.exe" (
  %PY% -m venv "%VENV%"
  if errorlevel 1 (
    echo Failed to create venv.
    exit /b 1
  )
)

echo [2/4] Installing PyInstaller...
"%VENV%\Scripts\python.exe" -m pip install --upgrade pip >nul
"%VENV%\Scripts\python.exe" -m pip install "pyinstaller>=6.0" 
if errorlevel 1 (
  echo pip install failed.
  exit /b 1
)

echo [3/4] Building FSRScaleUI.exe ...
if not exist "%BUILDDIR%" mkdir "%BUILDDIR%"

"%VENV%\Scripts\python.exe" -m PyInstaller ^
  --noconfirm ^
  --clean ^
  --windowed ^
  --onefile ^
  --name FSRScaleUI ^
  --distpath "%DISTDIR%" ^
  --workpath "%BUILDDIR%\work" ^
  --specpath "%BUILDDIR%" ^
  "%~dp0scale_ui.py"

if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

echo [4/4] Copying profiles template...
if not exist "%DISTDIR%\profiles" mkdir "%DISTDIR%\profiles"
if exist "%~dp0profiles\Default.json" (
  copy /Y "%~dp0profiles\Default.json" "%DISTDIR%\profiles\Default.json" >nul
)

echo.
echo Done.
echo   Output: %DISTDIR%\FSRScaleUI.exe
echo.
echo Place FSRScale.exe in the same folder as FSRScaleUI.exe
echo (default path is that folder), or pick it in the UI.
echo.
endlocal
exit /b 0
