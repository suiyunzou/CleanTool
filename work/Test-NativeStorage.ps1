param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\scratch\native-build\StorageDashboard.exe'),
    [switch]$RealScan
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$exe = [IO.Path]::GetFullPath($Executable)
[void][IO.Directory]::CreateDirectory((Join-Path $project 'outputs'))
function Get-LogInventory {
    @(Get-ChildItem -LiteralPath (Join-Path $project 'logs') -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName | ForEach-Object {
            $_.FullName + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }) -join "`n"
}
$before = Get-LogInventory
$cases = @(
    @('--self-test', 'native-core-tests.txt'),
    @('--hidden-ui-test', 'native-hidden-ui-tests.txt')
)
if ($RealScan) { $cases += ,@('--profile-readonly', 'native-real-scan.txt') }
foreach ($case in $cases) {
    $report = Join-Path $project ('outputs\' + $case[1])
    $process = Start-Process -FilePath $exe -ArgumentList @($case[0], ('"' + $report + '"')) -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit(45000)) {
            $process.Kill($true)
            throw "Test timeout: $($case[0])"
        }
        Get-Content -LiteralPath $report
        if ($process.ExitCode -ne 0) { throw "Test failed: $($case[0]); exit $($process.ExitCode)" }
    } finally { $process.Dispose() }
}
if ((Get-LogInventory) -cne $before) { throw 'Existing log inventory changed during tests' }
'PASS existing logs unchanged'
