@echo off
powershell -ExecutionPolicy Bypass -File scripts\fetch_deps.ps1
if errorlevel 1 exit /b %errorlevel%
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -FfxSdk "third_party\FidelityFX-SDK" -FfxBin "third_party\ffx-bin"
exit /b %errorlevel%
