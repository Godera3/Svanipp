param(
    [ValidateSet("Debug", "Release")]
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$ucrtBin = "C:\msys64\ucrt64\bin"
if (-not (Test-Path $ucrtBin)) {
    throw "MSYS2 UCRT64 toolchain not found at C:\msys64\ucrt64\bin. Run .\scripts\setup-windows.ps1 first."
}

$env:Path = "$ucrtBin;$env:Path"

function Assert-CommandExists {
    param(
        [string]$Name,
        [string]$Hint
    )

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name was not found in PATH. $Hint"
    }
}

Assert-CommandExists -Name "cmake" -Hint "Install CMake or run .\scripts\setup-windows.ps1."
Assert-CommandExists -Name "ninja" -Hint "Install Ninja with MSYS2 UCRT64 (run .\scripts\setup-windows.ps1 if needed)."

Write-Host "Configuring CMake ($BuildType)..."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=$BuildType -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++

Write-Host "Building..."
cmake --build build --config $BuildType -j

Write-Host ""
Write-Host "Build complete: build\\svanipp.exe"
Write-Host "Sanity check: .\\build\\svanipp.exe check"
