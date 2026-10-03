param(
  [Parameter(Mandatory=$true)]
  [string]$FfxSdk
)

$ErrorActionPreference = "Stop"

# Strip accidental trailing tokens (e.g. user typed ...1.1.4"clear)
$FfxSdk = $FfxSdk.Trim().TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $FfxSdk)) {
  Write-Error "FFX SDK path does not exist: $FfxSdk"
  exit 1
}

$root = Split-Path -Parent $PSScriptRoot
Write-Host "NRLive root : $root"
Write-Host "FFX SDK       : $FfxSdk"
Write-Host ""

# Quick layout check
$hdr = Join-Path $FfxSdk "ffx-api\include\ffx_api\ffx_api.h"
$dll = Join-Path $FfxSdk "PrebuiltSignedDLL\amd_fidelityfx_dx12.dll"
$dll2 = Join-Path $FfxSdk "ffx-api\bin\amd_fidelityfx_dx12.dll"
if (-not (Test-Path -LiteralPath $hdr)) {
  Write-Host "NOTE: $hdr not found."
  Write-Host "      CMake will also search PrebuiltSignedDLL / include / Kits layouts."
}
if (-not (Test-Path -LiteralPath $dll) -and -not (Test-Path -LiteralPath $dll2)) {
  Write-Host "NOTE: amd_fidelityfx_dx12.dll not under PrebuiltSignedDLL or ffx-api\bin."
}
Write-Host ""

cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 "-DFFX_SDK=$FfxSdk"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Built: $root\build\Release\NRLive.exe"
Write-Host "Ensure amd_fidelityfx_dx12.dll is beside the EXE (POST_BUILD copies it when found)."
Write-Host "If using OptiScaler, copy its dxgi.dll and OptiScaler.ini beside the EXE."
