param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\scratch\native-build\StorageDashboard.exe'),
    [string]$MsvcPath,
    [string]$WindowsSdkPath,
    [string]$WindowsSdkVersion
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $MsvcPath) {
    if ($env:VCToolsInstallDir) {
        $MsvcPath = $env:VCToolsInstallDir
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++, or specify -MsvcPath.' }
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'No MSVC x64 toolset found. Specify -MsvcPath.' }
        $versionFile = Join-Path $installation 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
        $toolVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
        $MsvcPath = Join-Path $installation "VC\Tools\MSVC\$toolVersion"
    }
}
if (-not $WindowsSdkPath) {
    if ($env:WindowsSdkDir) {
        $WindowsSdkPath = $env:WindowsSdkDir
    } else {
        $WindowsSdkPath = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10).KitsRoot10
    }
}
$msvc = [IO.Path]::GetFullPath($MsvcPath).TrimEnd('\')
$sdk = [IO.Path]::GetFullPath($WindowsSdkPath).TrimEnd('\')
if (-not $WindowsSdkVersion) {
    $WindowsSdkVersion = Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory |
        Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h')) -and
            (Test-Path -LiteralPath (Join-Path $sdk "Lib\$($_.Name)\um\x64\kernel32.lib")) -and
            (Test-Path -LiteralPath (Join-Path $sdk "Lib\$($_.Name)\ucrt\x64\ucrt.lib")) -and
            (Test-Path -LiteralPath (Join-Path $sdk "bin\$($_.Name)\x64\rc.exe")) } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
}
if (-not $WindowsSdkVersion) { throw 'No complete Windows SDK x64 installation found. Specify -WindowsSdkPath and -WindowsSdkVersion.' }
$sdkVersion = $WindowsSdkVersion.TrimEnd('\')
foreach ($required in @("$msvc\bin\Hostx64\x64\cl.exe", "$sdk\bin\$sdkVersion\x64\rc.exe", "$sdk\Include\$sdkVersion\um\Windows.h")) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required build tool or header missing: $required" }
}
$build = Join-Path $project 'scratch\native-build\x64'
[void][IO.Directory]::CreateDirectory($build)
$output = [IO.Path]::GetFullPath($OutputPath)
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output))
$compile = @('/nologo', '/std:c++17', '/O1', '/MT', '/EHsc', '/W4', '/utf-8', '/DUNICODE', '/D_UNICODE', '/D_WIN32_WINNT=0x0A00',
    "/I$msvc\include", "/I$sdk\Include\$sdkVersion\ucrt", "/I$sdk\Include\$sdkVersion\um", "/I$sdk\Include\$sdkVersion\shared")
$link = @('/link', '/SUBSYSTEM:WINDOWS', '/ENTRY:wmainCRTStartup', '/OPT:REF', '/OPT:ICF', '/DYNAMICBASE', '/NXCOMPAT',
    "/LIBPATH:$msvc\lib\x64", "/LIBPATH:$sdk\Lib\$sdkVersion\ucrt\x64", "/LIBPATH:$sdk\Lib\$sdkVersion\um\x64",
    '/MANIFEST:EMBED')
$previousPath = $env:PATH
try {
    $env:PATH = "$sdk\bin\$sdkVersion\x64;$msvc\bin\Hostx64\x64;$previousPath"
    $iconResource = Join-Path $build 'StorageDashboardNative.res'
    & "$sdk\bin\$sdkVersion\x64\rc.exe" /nologo "/I$project\src" "/fo$iconResource" "$project\src\StorageDashboardNative.rc"
    if ($LASTEXITCODE -ne 0) { throw "Icon resource build failed: $LASTEXITCODE" }
    foreach ($worker in @($true, $false)) {
        $obj = Join-Path $build $(if ($worker) { 'worker' } else { 'ui' })
        [void][IO.Directory]::CreateDirectory($obj)
        $target = if ($worker) { Join-Path ([IO.Path]::GetDirectoryName($output)) 'StorageWorker.exe' } else { $output }
        $arguments = $compile + @("/Fo$obj\", "/Fe$target")
        $names = if ($worker) { @('main', 'process', 'scanner', 'rules', 'system-cleanup') } else { @('main', 'process', 'scanner', 'rules', 'system-cleanup', 'ui', 'tests') }
        if ($worker) { $arguments += '/DSTORAGE_WORKER' }
        $arguments += @($names | ForEach-Object { Join-Path $project "src\native\$_.cpp" })
        if (-not $worker) { $arguments += $iconResource }
        $arguments += $link
        # Only the UI requests elevation. The worker inherits its parent's token and
        # must remain launchable with CreateProcess (including non-admin audits).
        if (-not $worker) {
            $arguments += @("/MANIFESTINPUT:$project\src\StorageDashboardNative.manifest", "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
        }
        $arguments += @('ole32.lib', 'advapi32.lib')
        if (-not $worker) { $arguments += @('user32.lib', 'gdi32.lib', 'psapi.lib') }
        & "$msvc\bin\Hostx64\x64\cl.exe" @arguments
        if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
    }
} finally { $env:PATH = $previousPath }
Get-Item -LiteralPath $output, (Join-Path ([IO.Path]::GetDirectoryName($output)) 'StorageWorker.exe') | Select-Object FullName, Length
