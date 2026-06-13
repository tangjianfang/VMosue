<#
.SYNOPSIS
Bootstrap VMosue build environment.
#>
[CmdletBinding()]
param(
    [string]$VcpkgRoot = "$env:USERPROFILE\vcpkg"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $VcpkgRoot)) {
    Write-Host "Cloning vcpkg to $VcpkgRoot..."
    git clone https://github.com/microsoft/vcpkg $VcpkgRoot
    & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
}

$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "VCPKG_ROOT=$VcpkgRoot"
Write-Host "Now run:"
Write-Host "  cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=`"$VcpkgRoot/scripts/buildsystems/vcpkg.cmake`""
Write-Host "  cmake --build build --config Release"