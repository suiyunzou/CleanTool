param([string]$OutputPath = (Join-Path $PSScriptRoot '..\scratch\native-build\StorageDashboard.exe'))
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Build-NativeStorage.ps1') -OutputPath $OutputPath
