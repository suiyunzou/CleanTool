if(-not ('CacheTransfer' -as [type])){Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'CacheTransfer.cs')))}
function Migration-Spec($Name) {
    switch($Name){
        'pip'{[pscustomobject]@{Name='pip';Exe='pip.exe';Variable='PIP_CACHE_DIR';Default=(Join-Path $env:LOCALAPPDATA 'pip\cache');Args=@('cache','dir')}}
        'npm'{[pscustomobject]@{Name='npm';Exe='npm.cmd';Variable='npm_config_cache';Default=(Join-Path $env:LOCALAPPDATA 'npm-cache');Args=@('config','get','cache','--logs-max=0','--update-notifier=false')}}
        'uv'{[pscustomobject]@{Name='uv';Exe='uv.exe';Variable='UV_CACHE_DIR';Default=(Join-Path $env:LOCALAPPDATA 'uv\cache');Args=@('cache','dir')}}
        default{throw '不支持的缓存'}
    }
}
function Migration-StatePath($Name) {
    [void](Migration-Spec $Name)
    Join-Path $env:LOCALAPPDATA ('CDriveStorageTool\migrations\'+$Name+'.json')
}
function Read-CacheSetting($Variable) {
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    try{
        if($key -and $key.GetValueNames() -contains $Variable){return [pscustomobject]@{Exists=$true;Value=$key.GetValue($Variable,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames);Kind=$key.GetValueKind($Variable).ToString()}}
        return [pscustomobject]@{Exists=$false;Value=$null;Kind='String'}
    }finally{if($key){$key.Dispose()}}
}
function Write-CacheSetting($Variable,$Value,[bool]$Exists=$true,$Kind='String') {
    $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment')
    try{if($Exists){$key.SetValue($Variable,[string]$Value,[Microsoft.Win32.RegistryValueKind]$Kind)}else{$key.DeleteValue($Variable,$false)}}finally{$key.Dispose()}
    [CacheTransfer]::NotifyEnvironment()
}
function Read-Migration($Name) {
    $p=Migration-StatePath $Name
    if(Test-Path -LiteralPath $p){[CacheTransfer]::Plain($p);Get-Content -LiteralPath $p -Raw | ConvertFrom-Json}
}
function Save-Migration($Journal) {
    $p=Migration-StatePath $Journal.Name;[CacheTransfer]::Plain($p)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($p))
    $tmp=$p+'.tmp';$text=$Journal | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($tmp,$text,[Text.UTF8Encoding]::new($true))
    if([IO.File]::Exists($p)){[IO.File]::Replace($tmp,$p,($p+'.previous'),$true)}else{[IO.File]::Move($tmp,$p)}
}
function Migration-Mutex {
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $m=[Threading.Mutex]::new($false,('Local\CDriveStorageMigration-'+$sid))
    try{$got=$m.WaitOne(0)}catch [Threading.AbandonedMutexException]{$got=$true}
    if(-not $got){$m.Dispose();throw '另一个窗口正在迁移，请等待完成'}
    return $m
}
function Cache-Busy($Spec,$Root) {
    foreach($p in @(Get-CimInstance Win32_Process -Filter "Name='uv.exe' OR Name='pip.exe' OR Name='python.exe' OR Name='pythonw.exe' OR Name='node.exe'" -ErrorAction Stop)){
        $line=[string]$p.CommandLine
        if(-not $line -and (($Spec.Name -eq 'npm' -and $p.Name -eq 'node.exe') -or ($Spec.Name -eq 'pip' -and $p.Name -like 'python*'))){return $true}
        if(($Spec.Name -eq 'uv' -and $p.Name -eq 'uv.exe') -or
           ($Spec.Name -eq 'pip' -and ($p.Name -eq 'pip.exe' -or ($p.Name -like 'python*' -and $line -match '(?i)(-m\s+pip\b|pip[\\/].*__main__)'))) -or
           ($Spec.Name -eq 'npm' -and $p.Name -eq 'node.exe' -and ($line -match '(?i)(npm-cli|npx-cli)\.js' -or $line.IndexOf($Root,[StringComparison]::OrdinalIgnoreCase) -ge 0))){return $true}
    }
    return $false
}
function Tool-CachePath($Spec) {
    $cmd=Get-Command $Spec.Exe -ErrorAction Stop
    # A fresh child reads persisted User/Machine values, not this launcher's stale environment.
    $exe=$cmd.Source.Replace("'","''");$variable=$Spec.Variable
    $argText=($Spec.Args | ForEach-Object {"'"+$_.Replace("'","''")+"'"}) -join ','
    $code="[Console]::OutputEncoding=[Text.UTF8Encoding]::new(`$false);`$v=[Environment]::GetEnvironmentVariable('$variable','User');if(`$null -eq `$v){`$v=[Environment]::GetEnvironmentVariable('$variable','Machine')};[Environment]::SetEnvironmentVariable('$variable',`$v,'Process'); & '$exe' @($argText);exit `$LASTEXITCODE"
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $info.Arguments='-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $info.WorkingDirectory=$env:USERPROFILE;$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $info.StandardOutputEncoding=[Text.Encoding]::UTF8
    $p=[Diagnostics.Process]::new();$p.StartInfo=$info
    try{
        [void]$p.Start();$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
        if(-not $p.WaitForExit(30000)){$p.Kill();throw '读取工具配置超时'}
        $output=$stdout.Result;$errorText=$stderr.Result
        if($p.ExitCode -ne 0){throw '工具无法读取缓存配置'}
        $path=@($output -split '\r?\n' | Where-Object {$_ -match '^[a-zA-Z]:[\\/]'}) | Select-Object -Last 1
        if(-not $path){throw '工具没有返回缓存路径'}
        return [IO.Path]::GetFullPath($path.Trim()).TrimEnd('\')
    }finally{$p.Dispose()}
}
function Migration-Drives {
    foreach($d in [IO.DriveInfo]::GetDrives()){
        if($d.IsReady -and $d.DriveType -eq 'Fixed' -and $d.Name -ine 'C:\' -and $d.DriveFormat -eq 'NTFS'){
            [pscustomobject]@{Root=$d.Name;FreeBytes=$d.AvailableFreeSpace;Label=($d.Name.Substring(0,2)+' · 可用 '+(Size-Text $d.AvailableFreeSpace))}
        }
    }
}
function Migration-List {
    if(-not ('StorageMeasure' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'StorageMeasure.cs')}
    foreach($name in @('pip','npm','uv')){
        if(Test-Cancel){break}
        Report-State ('正在检查 '+$name)
        $r=[pscustomobject]@{Name=$name;Root='';Bytes=$null;Complete=$false;State='未安装';Enabled=$false;Recovery=$false;Note=''}
        try{
            $s=Migration-Spec $name;$j=Read-Migration $name
            if($j -and $j.Phase -notin @('Complete','RolledBack')){
                $r.Root=$j.Source;$r.State='需要恢复';$r.Recovery=$true;$r.Enabled=$true;$r.Note=$j.Error
            }else{
                if(-not (Get-Command $s.Exe -ErrorAction SilentlyContinue)){$r;continue}
                $r.Root=Tool-CachePath $s
                if($j -and $j.Phase -eq 'Complete' -and $r.Root -ieq $j.Destination){$r.State='已迁至 '+[IO.Path]::GetPathRoot($r.Root).Substring(0,2)}
                elseif($r.Root -ine $s.Default){$r.State='自定义位置'}
                elseif(-not (Test-Path -LiteralPath $r.Root -PathType Container)){$r.State='缓存为空'}
                elseif(Cache-Busy $s $r.Root){$r.State='使用中';$r.Note='请先关闭使用此缓存的开发工具，再重新检查。'}
                else{
                    $m=[StorageMeasure]::Read($r.Root,1500)
                    $r.Bytes=$m.Bytes;$r.Complete=$m.Complete
                    if($m.Skipped -gt 0){$r.State='访问受限';$r.Note='存在访问限制或链接，未计为可迁移。'}
                    elseif($m.Bytes -eq 0 -and $m.Complete){$r.State='缓存为空'}
                    else{$r.State=if($m.Complete){'待迁移'}else{'待完整核对'};$r.Enabled=$true}
                }
            }
        }catch{$r.State='无法检查';$r.Note=$_.Exception.Message}
        $r
    }
}
function Assert-MigrationJournal($j) {
    $s=Migration-Spec $j.Name
    if($j.Source -ine $s.Default -or $j.Id -notmatch '^[a-f0-9]{32}$'){throw '迁移记录路径不符合范围'}
    $expectedBackup=$s.Default+'.storage-'+$j.Id
    $drive=[IO.Path]::GetPathRoot($j.Destination)
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $expectedDest=Join-Path $drive ('DevCaches\'+$sid+'\'+$j.Name+'-'+$j.Id)
    if($drive -ieq 'C:\' -or $drive -notmatch '^[A-Z]:\\$' -or $j.Backup -ine $expectedBackup -or $j.Destination -ine $expectedDest){throw '迁移记录目标不符合范围'}
    foreach($p in @($j.Source,$j.Backup,$j.Destination)){[CacheTransfer]::Plain($p)}
    return $s
}
function Journal-Files($j) {
    [CacheFile[]]@(foreach($f in $j.Files){
        $c=[CacheFile]::new();$c.Name=$f.Name;$c.Length=$f.Length;$c.Hash=$f.Hash;$c.Created=[datetime]$f.Created;$c.Modified=[datetime]$f.Modified;$c
    })
}
function Restore-Migration($j) {
    $s=Assert-MigrationJournal $j;$files=Journal-Files $j
    $current=Read-CacheSetting $s.Variable
    if($current.Value -cne $j.OldUser -and $current.Value -cne $j.Destination -and -not (-not $current.Exists -and -not $j.OldUser)){throw '缓存配置已被其他程序修改，未覆盖；可查看迁移记录'}
    $j.Phase='RollingBack';Save-Migration $j;Report-State '正在恢复原文件和配置，请稍候'
    $restoreReport=[Action[int,long]]{param($count,$size) Report-State ('正在恢复 '+$count+' / '+$files.Count+' · '+(Size-Text $size))}
    if(Test-Path -LiteralPath $j.Backup){
        if(Test-Path -LiteralPath $j.Source){
            if([IO.Directory]::GetFileSystemEntries($j.Source).Count -eq 0){[IO.Directory]::Delete($j.Source,$false)}
            else{throw '原位置已生成新缓存，两份数据均已保留；请关闭开发工具后处理'}
        }
        [CacheTransfer]::Restore($j.Backup,$j.Destination,$files,$restoreReport)
        [IO.Directory]::Move($j.Backup,$j.Source)
    }elseif(-not (Test-Path -LiteralPath $j.Source)){
        [CacheTransfer]::Restore($j.Backup,$j.Destination,$files,$restoreReport)
        [IO.Directory]::Move($j.Backup,$j.Source)
    }
    Report-State '正在核对恢复后的文件'
    [CacheTransfer]::Match($j.Source,$files)
    $existed=if($null -ne $j.OldUserExists){[bool]$j.OldUserExists}else{[bool]$j.OldUser}
    $kind=if($j.OldUserKind){$j.OldUserKind}else{'String'}
    Write-CacheSetting $s.Variable $j.OldUser $existed $kind
    [Environment]::SetEnvironmentVariable($s.Variable,$j.OldProcess,'Process')
    $restored=Read-CacheSetting $s.Variable
    if($restored.Exists -ne $existed -or ($existed -and ($restored.Value -cne $j.OldUser -or $restored.Kind -ne $kind))){throw '原配置尚未恢复'}
    if((Tool-CachePath $s) -ine $j.Source){throw '文件已恢复，工具配置尚未恢复，请查看记录'}
    $j.Phase='RolledBack';$j.RemovedSourceBytes=0L;$j.RollbackVerified=$true
    # Keep a changed/partial destination for investigation; delete only an exact verified copy.
    if(Test-Path -LiteralPath $j.Destination){
        Report-State '原位已恢复，正在核对并移除目标副本'
        try{[CacheTransfer]::RemoveOwnedSubset($j.Destination,$files);$j.TargetRetained=$false}catch{$j.TargetRetained=$true}
    }
    Save-Migration $j
    return $j
}
function Recover-Migration($Name) {
    $mutex=Migration-Mutex
    try{
        $j=Read-Migration $Name;if(-not $j){throw '没有可恢复的记录'}
        $s=Assert-MigrationJournal $j
        if((Cache-Busy $s $j.Source) -or (Cache-Busy $s $j.Destination)){throw '开发工具仍在使用缓存，请关闭后恢复'}
        try{Restore-Migration $j}catch{$j.Phase='RecoveryRequired';$j.Error=$_.Exception.Message;Save-Migration $j;return $j}
    }finally{$mutex.ReleaseMutex();$mutex.Dispose()}
}
function Start-CacheMigration($Name,$Drive,[scriptblock]$TestFault=$null) {
    $mutex=Migration-Mutex;$j=$null
    try{
        $s=Migration-Spec $Name;$old=Read-Migration $Name
        if($old -and $old.Phase -notin @('Complete','RolledBack')){throw '先恢复上次未完成的迁移'}
        $root=Tool-CachePath $s
        if($root -ine $s.Default){throw '本版只迁移默认位置的缓存'}
        if(Cache-Busy $s $root){throw '缓存正在使用，请关闭相关开发工具'}
        $drives=@(Migration-Drives | Where-Object Root -eq $Drive)
        if($drives.Count -ne 1){throw '请选择 C 盘以外的本机 NTFS 固定磁盘'}
        $setting=Read-CacheSetting $s.Variable;$user=$setting.Value;$process=[Environment]::GetEnvironmentVariable($s.Variable,'Process');$machine=[Environment]::GetEnvironmentVariable($s.Variable,'Machine')
        if($process -and $process -cne $user -and $process -cne $machine){throw '当前进程有独立缓存配置，请从新窗口启动后重试'}
        Report-State '正在核对源文件'
        $report=[Action[int,long]]{param($count,$size) Report-State ('正在校验 '+$count+' 个文件 · '+(Size-Text $size))}
        $cancel=[Func[bool]]{return [bool](Test-Cancel)}
        $files=[CacheTransfer]::Read($root,$report,$cancel);$bytes=[long](($files | Measure-Object Length -Sum).Sum)
        if($bytes -eq 0){throw '缓存为空，无需迁移'}
        if($drives[0].FreeBytes -lt ($bytes+256MB)){throw '目标盘空间不足，需保留至少 256 MiB 余量'}
        $id=[guid]::NewGuid().ToString('N');$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        $dest=Join-Path $Drive ('DevCaches\'+$sid+'\'+$Name+'-'+$id)
        $j=[pscustomobject]@{Operation='migration';Name=$Name;Id=$id;Time=(Get-Date).ToString('o');Phase='Prepared';Source=$root;Backup=($root+'.storage-'+$id);Destination=$dest;OldUser=$user;OldUserExists=$setting.Exists;OldUserKind=$setting.Kind;OldProcess=$process;Files=@($files);SourceBytes=$bytes;VerifiedBytes=0L;RemovedSourceBytes=0L;ReclaimedBytes=$null;RollbackVerified=$false;TargetRetained=$false;Error=''}
        [void](Assert-MigrationJournal $j)
        if((Test-Path -LiteralPath $dest) -or (Test-Path -LiteralPath $j.Backup)){throw '目标已存在，未覆盖'}
        Save-Migration $j
        if(Test-Cancel){throw '已取消'}
        # Rename first: all failures retain or restore the original source, including crashes.
        [IO.Directory]::Move($root,$j.Backup);$j.Phase='SourceHeld';Save-Migration $j
        [void][IO.Directory]::CreateDirectory($dest)
        # The new cache is private to this user, Administrators and SYSTEM, even on a shared data drive.
        $acl=Get-Acl -LiteralPath $dest
        $acl.SetAccessRuleProtection($true,$false)
        foreach($identity in @($sid,'S-1-5-18','S-1-5-32-544')){
            $rule=[Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($identity),'FullControl','ContainerInherit, ObjectInherit','None','Allow')
            $acl.AddAccessRule($rule)
        }
        Set-Acl -LiteralPath $dest -AclObject $acl
        Report-State '正在复制并逐文件校验'
        $copyReport=[Action[int,long]]{param($count,$size) Report-State ('已复制校验 '+$count+' / '+$files.Count+' · '+(Size-Text $size))}
        [CacheTransfer]::Copy($j.Backup,$dest,$files,$copyReport,$cancel)
        $j.VerifiedBytes=$bytes;$j.Phase='Copied';Save-Migration $j
        if(Test-Cancel){throw '已取消'}
        if(Test-Path -LiteralPath $root){throw '有程序重新创建了旧缓存，已停止迁移'}
        if(Cache-Busy $s $root){throw '迁移期间启动了开发工具，已停止迁移'}
        # Journal precedes configuration so interruption can always restore its exact old value.
        $j.Phase='Configuring';Save-Migration $j
        Write-CacheSetting $s.Variable $dest
        [Environment]::SetEnvironmentVariable($s.Variable,$dest,'Process')
        if($TestFault){& $TestFault 'Configured' $j}
        Report-State '正在验证工具的新位置'
        if([Environment]::GetEnvironmentVariable($s.Variable,'User') -cne $dest -or (Tool-CachePath $s) -ine $dest){throw '工具仍未使用目标缓存'}
        $probe=Join-Path $dest ('.storage-probe-'+$id)
        try{[IO.File]::WriteAllText($probe,$id);if([IO.File]::ReadAllText($probe) -cne $id){throw '目标缓存读写校验失败'}}finally{if([IO.File]::Exists($probe)){[IO.File]::Delete($probe)}}
        [CacheTransfer]::Match($dest,$files)
        if(Test-Cancel){throw '已取消'}
        if(Test-Path -LiteralPath $root){throw '原缓存被重新创建，已停止提交'}
        $j.Phase='RemovingOld';Save-Migration $j
        Report-State '验证通过，正在移除旧副本'
        $j.RemovedSourceBytes=[CacheTransfer]::Remove($j.Backup,$files)
        if($TestFault){& $TestFault 'OldRemoved' $j}
        if(Test-Cancel){throw '已取消'}
        $j.Phase='Complete';Save-Migration $j
        return $j
    }catch{
        $message=$_.Exception.Message
        if(-not $j){throw}
        $j.Error=$message
        try{return (Restore-Migration $j)}catch{$j.Phase='RecoveryRequired';$j.Error=$message+'；恢复未完成：'+$_.Exception.Message;Save-Migration $j;return $j}
    }finally{$mutex.ReleaseMutex();$mutex.Dispose()}
}
