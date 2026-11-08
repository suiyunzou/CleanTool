param([ValidateSet('Menu','Audit','AutoTemp','SelfTest')][string]$Mode='Menu',[switch]$Demo)
$ErrorActionPreference='Stop'
if($Demo -and $Mode -ne 'Menu'){throw '演练只支持窗口模式'}
if($Mode -eq 'Menu'){
    $start=@{FilePath=(Join-Path $PSScriptRoot 'StorageDashboard.exe')}
    if($Demo){$start.ArgumentList='--demo'}
    Start-Process @start
    exit
}
$binary=if($Mode -eq 'SelfTest'){'StorageDashboard.exe'}else{'StorageWorker.exe'}
$argument=if($Mode -eq 'SelfTest'){
    $report=Join-Path $PSScriptRoot '..\outputs\native-core-tests.txt'
    '--self-test "'+[IO.Path]::GetFullPath($report)+'"'
}elseif($Mode -eq 'Audit'){'--audit'}else{'--auto-temp'}
$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $PSScriptRoot $binary),$argument)
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true
$start.RedirectStandardError=$true
$start.StandardOutputEncoding=[Text.Encoding]::UTF8
$process=[Diagnostics.Process]::Start($start)
try {
    $output=$process.StandardOutput.ReadToEndAsync()
    $errors=$process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $text=$output.GetAwaiter().GetResult()
    $errorText=$errors.GetAwaiter().GetResult()
    if($text){Write-Output $text.Trim()}
    if($errorText){Write-Warning $errorText.Trim()}
    if($Mode -eq 'SelfTest'){Get-Content -LiteralPath $report}
    exit $process.ExitCode
}finally{$process.Dispose()}
