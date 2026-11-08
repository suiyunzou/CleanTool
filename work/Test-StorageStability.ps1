param([switch]$RealScan)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Test-NativeStorage.ps1') -RealScan:$RealScan
