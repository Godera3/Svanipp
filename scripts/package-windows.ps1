param(
    [ValidateSet("Debug", "Release")]
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$ucrtBin = "C:\msys64\ucrt64\bin"
$runtimeDlls = @(
    "libstdc++-6.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll"
)

# Ensure build exists
if (-not (Test-Path ".\build\svanipp.exe")) {
    Write-Host "Build not found; building..."
    & .\scripts\build-windows.ps1 -BuildType $BuildType
}

# Create distribution folder
$dist = Join-Path $repoRoot "dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null

Copy-Item -Path ".\build\svanipp.exe" -Destination (Join-Path $dist "svanipp.exe") -Force

# Copy runtime DLLs if available for portability
if (Test-Path $ucrtBin) {
    foreach ($dll in $runtimeDlls) {
        $source = Join-Path $ucrtBin $dll
        if (Test-Path $source) {
            Copy-Item -Path $source -Destination $dist -Force
        }
    }
}

# Optionally include README and LICENSE if present
if (Test-Path ".\README.md") { Copy-Item .\README.md -Destination $dist -Force }
if (Test-Path ".\LICENSE") { Copy-Item .\LICENSE -Destination $dist -Force }

# Zip distribution
$zipPath = Join-Path $repoRoot "dist\svanipp-windows.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path "$dist\*" -DestinationPath $zipPath

Write-Host "Created: $zipPath"
Write-Host "Also available: $dist\svanipp.exe"
