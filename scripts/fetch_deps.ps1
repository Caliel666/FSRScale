$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdkPath = Join-Path $root "third_party\FidelityFX-SDK"
$binPath = Join-Path $root "third_party\ffx-bin"
$runtimeZip = Join-Path $env:TEMP "FidelityFX-Samples-v2.3.0-prebuilt.zip"
$runtimeUrl = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.3.0/FidelityFX-Samples-v2.3.0-prebuilt.zip"

if (!(Test-Path (Join-Path $sdkPath "Kits\FidelityFX\api\include\ffx_api.h"))) {
  if (Test-Path $sdkPath) { Remove-Item -LiteralPath $sdkPath -Recurse -Force }
  New-Item -ItemType Directory -Force -Path (Split-Path $sdkPath) | Out-Null
  Write-Host "Fetching AMD FSR SDK 2.3.0 headers..."
  git clone --depth 1 --branch v2.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $sdkPath
  if ($LASTEXITCODE -ne 0) { throw "Failed to clone FSR SDK 2.3.0." }
}
if (!(Test-Path (Join-Path $binPath "amd_fidelityfx_loader_dx12.dll"))) {
  if (Test-Path $binPath) { Remove-Item -LiteralPath $binPath -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $binPath | Out-Null
  Write-Host "Fetching AMD FSR SDK 2.3.0 signed runtime providers..."
  curl.exe -L --fail --retry 5 --retry-delay 3 --output $runtimeZip $runtimeUrl
  if ($LASTEXITCODE -ne 0) { throw "Failed to download FSR SDK 2.3.0 runtime." }
  $extract = Join-Path $env:TEMP "NRLive-fsr-runtime"
  if (Test-Path $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
  Expand-Archive -LiteralPath $runtimeZip -DestinationPath $extract -Force
  foreach ($dllName in @(
    "amd_fidelityfx_loader_dx12.dll",
    "amd_fidelityfx_framegeneration_dx12.dll",
    "amd_fidelityfx_upscaler_dx12.dll"
  )) {
    $found = Get-ChildItem -Path $extract -Recurse -File -Filter $dllName | Select-Object -First 1
    if (-not $found) { throw "FSR SDK 2.3.0 runtime DLL missing: $dllName" }
    Copy-Item -LiteralPath $found.FullName -Destination (Join-Path $binPath $dllName) -Force
  }
}
Write-Host "FSR SDK source: $sdkPath"
Write-Host "FSR runtime DLLs: $binPath"
Write-Host "Run scripts/build.ps1 -FfxSdk third_party/FidelityFX-SDK -FfxBin third_party/ffx-bin"
