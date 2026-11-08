param([switch]$Audit,[string]$DataPath)
$ErrorActionPreference='Stop'
$arguments=@()
if($Audit){$arguments+='--audit'}
if($DataPath){
    $path=[IO.Path]::GetFullPath($DataPath)
    if($path.Contains('"')){throw '记录路径无效'}
    $arguments+=@('--data-path',('"'+$path.TrimEnd('\')+'"'))
}
$binary=if($Audit){'StorageWorker.exe'}else{'StorageDashboard.exe'}
$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $PSScriptRoot $binary),($arguments -join ' '))
$start.UseShellExecute=$false
if(-not $Audit){[void][Diagnostics.Process]::Start($start);exit}
$start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true
$start.RedirectStandardError=$true
$start.StandardOutputEncoding=[Text.Encoding]::UTF8
$process=[Diagnostics.Process]::Start($start)
try{
    $output=$process.StandardOutput.ReadToEndAsync()
    $errors=$process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $text=$output.GetAwaiter().GetResult()
    $errorText=$errors.GetAwaiter().GetResult()
    if($text){Write-Output $text.Trim()}
    if($process.ExitCode -ne 0){throw $errorText}
}finally{$process.Dispose()}
