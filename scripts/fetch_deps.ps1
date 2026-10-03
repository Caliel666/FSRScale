$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dst = Join-Path $root "third_party\ffx"
if (Test-Path $dst) { Write-Host "FFX headers already present."; exit 0 }
New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
Write-Host "Fetching AMD FidelityFX SDK..."
git clone --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git (Join-Path $root "third_party\FidelityFX-SDK")
$source = Join-Path $root "third_party\FidelityFX-SDK\Kits\FidelityFX\api\include"
if (!(Test-Path $source)) { throw "FidelityFX SDK headers were not found." }
Copy-Item $source $dst -Recurse
Write-Host "FFX headers installed."
