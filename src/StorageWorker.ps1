$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
. (Join-Path $PSScriptRoot 'StorageCore.ps1')
. (Join-Path $PSScriptRoot 'StorageInsights.ps1')
. (Join-Path $PSScriptRoot 'StorageMigration.ps1')

function Say($Text,$Color='Gray') { if(-not $script:Shared){Write-Host $Text} }
function Report-State($Text) {if($script:Shared){$script:Shared.Status=$Text}}
function Test-Cancel {return ($script:Shared -and $script:Shared.Cancel)}
function Save-Record($Name,$Value) {
    if($script:DemoRecordPath){$Value | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $script:DemoRecordPath -Encoding UTF8;return $script:DemoRecordPath}
    $dir=Join-Path $PSScriptRoot 'logs';[void](New-Item -ItemType Directory -Path $dir -Force)
    $path=Join-Path $dir ((Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$Name+'.json')
    $Value | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}
function Run-External($Executable,$Arguments) {
    $prior=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try{$global:LASTEXITCODE=$null;$output=(& $Executable @Arguments 2>&1 | Out-String);$code=$global:LASTEXITCODE}finally{$ErrorActionPreference=$prior}
    if($null -eq $code){throw ('未取得工具退出码：'+$output.Trim())}
    [pscustomobject]@{Code=$code;Output=$output.Trim()}
}
function Row($Id,$Name,$Kind,$Targets,$Default=$false) {
    [pscustomobject]@{Id=$Id;Name=$Name;Kind=$Kind;Targets=@($Targets);Default=$Default;Bytes=0L;Eligible=0L;CandidateBytes=0L;Count=0;State='待扫描';Enabled=$false;Scans=@();Note='';Package=$null;CheckedAt='';SizeBasis='LogicalFileBytes';ReclaimableBytes=$null}
}
function Build-Rows([string]$DemoRoot='') {
    if($DemoRoot){
        Row 'temp' '旧临时文件（演练）' 'Temp' @(Target '演练临时文件' $DemoRoot 'Temp') $true
        return
    }
    $temps=@(Temp-Targets)
    Row 'user' '用户临时文件' 'Temp' @($temps[0]) $true
    Row 'windows' '系统临时文件' 'Temp' @($temps[1]) $true
    Row 'dump' '旧崩溃转储' 'Temp' @($temps[2])
    Row 'updater' '更新安装残留' 'Temp' @($temps[3])
    $shaders=@(Shader-Targets)
    Row 'directx' 'DirectX 着色器' 'Shader' @($shaders[0])
    Row 'nvidia' 'NVIDIA 着色器' 'Shader' @($shaders[1],$shaders[2])
    Row 'pip' 'pip 下载缓存' 'Package' @()
    Row 'npm' 'npm 下载缓存' 'Package' @()
    Row 'uv' 'uv 闲置缓存' 'Package' @()
    foreach($entry in @(@('chrome','Chrome','chrome'),@('edge','Edge','msedge'),@('code','VS Code','Code'),@('cursor','Cursor','Cursor'))){
        Row $entry[0] ($entry[1]+' 缓存') 'Browser' @()
    }
}
function Package-Info($Name) {
    $migration=Read-Migration $Name
    if($migration -and $migration.Phase -notin @('Complete','RolledBack')){throw '迁移未完成，请先在缓存迁移中恢复'}
    if($migration -and $migration.Phase -eq 'Complete' -and (Tool-CachePath (Migration-Spec $Name)) -ieq $migration.Destination){throw ('已迁出 C 盘，位于 '+[IO.Path]::GetPathRoot($migration.Destination))}
    $exeName=switch($Name){'pip'{'pip.exe'} 'npm'{'npm.cmd'} 'uv'{'uv.exe'}}
    $cmd=Get-Command $exeName -ErrorAction SilentlyContinue
    if(-not $cmd){throw '未安装'}
    $getArgs=switch($Name){'npm'{@('config','get','cache')} default{@('cache','dir')}}
    $reply=Run-External $cmd.Source $getArgs
    if($reply.Code -ne 0){throw '无法读取缓存目录'}
    $root=($reply.Output -split '\r?\n' | Where-Object {$_ -match '^[A-Za-z]:[\\/]'} | Select-Object -Last 1)
    if(-not $root){throw '缓存目录无法识别'}
    $expected=switch($Name){'pip'{Join-Path $env:LOCALAPPDATA 'pip\cache'} 'npm'{Join-Path $env:LOCALAPPDATA 'npm-cache'} 'uv'{Join-Path $env:LOCALAPPDATA 'uv\cache'}}
    if([IO.Path]::GetFullPath($root).TrimEnd('\') -ine [IO.Path]::GetFullPath($expected).TrimEnd('\')){throw '自定义路径，需单独处理'}
    if(Test-Path -LiteralPath $root){Check-PlainPath $root}
    $measure=$root;if($Name -eq 'npm'){$measure=Join-Path $root '_cacache'}
    $cleanArgs=switch($Name){'pip'{@('cache','purge')} 'npm'{@('cache','clean','--force')} 'uv'{@('cache','prune','--offline')}}
    [pscustomobject]@{Name=$Name;Executable=$cmd.Source;Root=$root;Measure=$measure;Arguments=$cleanArgs}
}
function Scan-Rows($Rows,[int]$Days) {
    $scanClock=[Diagnostics.Stopwatch]::StartNew()
    $maintenance=@(Get-MaintenanceState)
    foreach($row in $Rows){
        if(Test-Cancel){break}
        Report-State ('正在扫描 '+$row.Name)
        $copy=$row.PSObject.Copy();$copy.Scans=@();$copy.State='未发现';$copy.Enabled=$false;$copy.Bytes=0L;$copy.Eligible=0L;$copy.CandidateBytes=0L;$copy.Count=0;$copy.Note='';$copy.CheckedAt=(Get-Date).ToString('o')
        try{
            if($scanClock.ElapsedMilliseconds -ge 30000){throw '达到检查时限；此项目未检查'}
            if($row.Kind -eq 'Browser'){
                $processName=switch($row.Id){'chrome'{'chrome'} 'edge'{'msedge'} 'code'{'Code'} 'cursor'{'Cursor'}}
                $copy.Targets=@(Browser-Targets $processName)
            }
            if($row.Kind -eq 'Package'){
                $copy.Package=Package-Info $row.Id
                $copy.Targets=@(Target $row.Name $copy.Package.Measure 'Package')
            }
            $scans=@(foreach($target in $copy.Targets){Scan-Target $target $Days ([Math]::Max(1,[Math]::Min(8000,30000-$scanClock.ElapsedMilliseconds)))})
            $copy.Scans=$scans
            $copy.Bytes=[long](($scans | Measure-Object Bytes -Sum).Sum)
            $copy.Eligible=[long](($scans | Measure-Object Eligible -Sum).Sum)
            $copy.CandidateBytes=$copy.Eligible
            $copy.Count=[int](($scans | ForEach-Object {$_.Files.Count} | Measure-Object -Sum).Sum)
            $busy=@($copy.Targets | Where-Object {$_.Process -and (Get-Process -Name $_.Process -ErrorAction SilentlyContinue)}).Count -gt 0
            if($row.Kind -eq 'Package'){
                if($copy.Bytes -gt 0){$copy.State='可整理';$copy.Enabled=$true}else{$copy.State='缓存为空'}
                $recent=@($maintenance | Where-Object {$_.Id -eq $row.Id -and $_.Path -ieq $copy.Package.Measure -and ([datetime]$_.Time) -gt (Get-Date).AddDays(-7) -and $_.Fingerprint -eq (Cache-Fingerprint $scans)})
                if($recent.Count -gt 0 -and @($scans|Where-Object {$_.Skipped -gt 0}).Count -eq 0){$copy.State='近期无变化';$copy.Enabled=$false}
                $copy.Note='缓存占用不等于可释放量；由工具维护后核对大小变化。'
            }elseif($busy){$copy.State='使用中';$copy.Eligible=0L;$copy.CandidateBytes=0L;$copy.Note='应用正在运行，未计入待清理大小。关闭后重新检查。'}
            elseif($copy.Eligible -gt 0){$copy.State='可处理';$copy.Enabled=$true;$copy.Note='文件逻辑大小，不能作为可释放空间；实际删除时再检查占用和变化。'}
            elseif($scans.Count -gt 0){$copy.State='无候选文件'}
            $retained=@($scans | ForEach-Object {$_.Issues} | Where-Object {$_}).Count
            if($retained -gt 0 -and -not $busy -and $row.Kind -ne 'Package'){
                $copy.State=if($copy.Eligible -gt 0){'部分可处理'}else{'已保留'}
            }
            $errors=[int](($scans | Measure-Object Skipped -Sum).Sum)
            if($errors -gt 0){
                $copy.Note+=(' 已跳过 '+$errors+' 处链接、保留目录或访问限制，大小仅含已读取部分。')
                if(-not $busy){$copy.State='部分检查'}
            }
        }catch{
            if(Test-Cancel){break}
            $copy.State='不可用';$copy.Enabled=$false;$copy.Eligible=0L;$copy.CandidateBytes=0L;$copy.Note=$_.Exception.Message
            if($copy.Note -like '已迁出 C 盘*'){$copy.State='已迁出 C 盘'}
        }
        if($script:Shared){$script:Shared.Scanned.Add($copy)}else{$copy}
    }
}
function Measure-PackageBytes($Info) {
    $scan=Scan-Target (Target $Info.Name $Info.Measure 'Package') 7
    if($null -eq $scan){return 0L}
    if($scan.Skipped -gt 0){throw '缓存目录未完整读取，无法确认大小变化'}
    return [long]$scan.Bytes
}
function Clean-Rows($Rows) {
    $started=(Get-Date).ToString('o');$before=Free-Bytes;$records=[Collections.Generic.List[object]]::new();$deleted=0;$skipped=0;$completed=0;$failed=0;$unconfirmed=0
    [long]$packageReduced=0;$packageUnknown=0
    foreach($row in $Rows){
        if(Test-Cancel){break}
        Report-State ('正在清理 '+$row.Name+'；Esc 在当前步骤后停止')
        if($row.Kind -eq 'Package'){
            $cacheMutex=$null
            try{
                $cacheMutex=Migration-Mutex
                $info=Package-Info $row.Id
                if($info.Root -ine $row.Package.Root){throw '缓存位置已变化，请重新扫描'}
                $cacheBefore=$null;$cacheAfter=$null;$beforeState=$null;$afterState=$null;$measureError=''
                try{$beforeState=Measure-PackageState $info;if(-not $beforeState.Complete){throw '缓存未完整读取'};$cacheBefore=$beforeState.Bytes}catch{$measureError=$_.Exception.Message}
                if(Test-Cancel){break}
                $result=Run-External $info.Executable $info.Arguments
                try{$afterState=Measure-PackageState $info;if(-not $afterState.Complete){throw '缓存未完整读取'};$cacheAfter=$afterState.Bytes}catch{$measureError=$_.Exception.Message}
                Save-NoChange $row.Id $info $beforeState $afterState $result.Code
                $decrease=$null
                if($null -ne $cacheBefore -and $null -ne $cacheAfter){$decrease=[Math]::Max(0L,($cacheBefore-$cacheAfter));$packageReduced+=$decrease}else{$packageUnknown++}
                $records.Add([pscustomobject]@{Item=$row.Name;Command=($info.Arguments -join ' ');ExitCode=$result.Code;Output=$result.Output;MeasurePath=$info.Measure;SizeBasis='LogicalFileBytes';BeforeFingerprint=$beforeState.Fingerprint;AfterFingerprint=$afterState.Fingerprint;BeforeBytes=$cacheBefore;AfterBytes=$cacheAfter;ReducedBytes=$decrease;MeasureError=$measureError})
                if($result.Code -ne 0){$failed++}else{$completed++}
            }catch{$failed++;$packageUnknown++;$records.Add([pscustomobject]@{Item=$row.Name;Error=$_.Exception.Message;ReducedBytes=$null})}
            finally{if($cacheMutex){$cacheMutex.ReleaseMutex();$cacheMutex.Dispose()}}
        }else{
            foreach($scan in $row.Scans){
                foreach($issue in @($scan.Issues)){
                    if($issue){$skipped++;$records.Add([pscustomobject]@{Group=$scan.Target.Name;Path=$issue.Path;Bytes=0L;Status='Skipped';Reason=$issue.Reason})}
                }
                foreach($record in @(Delete-Candidates $scan $row.Targets)){
                    $records.Add($record)
                    if($record.Status -eq 'Deleted'){$deleted++}
                    elseif($record.Status -eq 'Unconfirmed'){$unconfirmed++}
                    else{$skipped++}
                }
            }
        }
    }
    $after=Free-Bytes
    $removed=[long](($records | Where-Object Status -eq 'Deleted' | Measure-Object Bytes -Sum).Sum)
    $outcome=[pscustomobject]@{Time=$started;FinishedAt=(Get-Date).ToString('o');Deleted=$deleted;Skipped=$skipped;Failed=$failed;Unconfirmed=$unconfirmed;Completed=$completed;RemovedBytes=$removed;PackageReducedBytes=$packageReduced;PackageUnknown=$packageUnknown;BeforeFree=$before;AfterFree=$after;DiskFreeDelta=($after-$before);ReclaimedBytes=$null;Canceled=[bool](Test-Cancel);SizeBasis='LogicalFileBytes'}
    $path=Save-Record 'cleanup' ([pscustomobject]@{Time=$started;BeforeFree=$before;AfterFree=$after;Canceled=$outcome.Canceled;Outcome=$outcome;Files=$records.ToArray()})
    $summary=('结果：确认删除 {0} 个文件（文件大小 {1}），跳过 {2} 项，失败 {3} 项，删除未确认 {4} 项；缓存命令正常退出 {5} 项，目录观测减少 {6}，大小未确认 {7} 项；C 盘可用空间净变化 {8:N2} MiB（非本次释放量）' -f $deleted,(Size-Text $removed),$skipped,$failed,$unconfirmed,$completed,(Size-Text $packageReduced),$packageUnknown,(($after-$before)/1MB))
    if($outcome.Canceled){$summary='已停止。'+$summary}
    $outcome | Add-Member -NotePropertyName Summary -NotePropertyValue $summary
    $outcome | Add-Member -NotePropertyName Log -NotePropertyValue $path
    return $outcome
}
function Cleanup-Display($Result) {
    $r=$Result;$top='清理结束';$large='本次未删除';$note='没有确认删除的文件'
    if($r.Deleted -gt 0){$top='已删除文件';$large=Size-Text $r.RemovedBytes;$note=('{0} 个文件，显示的是文件大小' -f $r.Deleted)}
    elseif($r.PackageReducedBytes -gt 0){$top='缓存目录减少';$large=Size-Text $r.PackageReducedBytes;$note='实测文件大小变化，不等于磁盘释放量'}
    elseif($r.Completed -gt 0){$top='整理结束';$large='缓存未减少';$note='工具已运行，目录大小没有减少'}
    if($r.Deleted -gt 0 -and $r.PackageReducedBytes -gt 0){$note='文件大小；缓存另减少 '+(Size-Text $r.PackageReducedBytes)}
    $issues=@()
    if($r.Skipped -gt 0){$issues+=('跳过 '+$r.Skipped+' 项')}
    if($r.Failed -gt 0){$issues+=('失败 '+$r.Failed+' 项')}
    if($r.PackageUnknown -gt 0 -or $r.Unconfirmed -gt 0){$issues+='部分结果未确认'}
    if($issues.Count -gt 0){
        $note=($issues -join '，')+'；点详情查看原因'
        if($r.Deleted -eq 0 -and $r.PackageReducedBytes -eq 0){
            if($r.Failed -gt 0){$large='未能完成'}
            elseif($r.PackageUnknown -gt 0 -or $r.Unconfirmed -gt 0){$large='结果未确认'}
        }
    }
    if($r.Canceled){$top='已停止';$note='以下为停止前结果；'+$note}
    [pscustomobject]@{Top=$top;Large=$large;Note=$note}
}
function Run-ComponentOperation($Operation) {
    $option=if($Operation -eq 'analyze'){'/AnalyzeComponentStore'}else{'/StartComponentCleanup'}
    Report-State $(if($Operation -eq 'analyze'){'正在分析组件存储'}else{'正在执行系统清理，请等待系统返回'})
    $before=Free-Bytes
    $r=Run-External "$env:SystemRoot\System32\dism.exe" @('/Online','/Cleanup-Image',$option,'/NoRestart')
    [pscustomobject]@{Operation=$Operation;Time=(Get-Date).ToString('o');ExitCode=$r.Code;Succeeded=($r.Code -in @(0,3010));RestartRequired=($r.Code -eq 3010);Output=$r.Output;DiskFreeDelta=((Free-Bytes)-$before);ReclaimedBytes=$null}
}
function Component-Display($Result) {
    if(-not $Result.Succeeded){return [pscustomobject]@{Top='系统操作未完成';Large='未能完成';Note=('退出码 '+$Result.ExitCode+'；可查看记录')}}
    if($Result.RestartRequired){return [pscustomobject]@{Top='系统操作已返回';Large='需要重启';Note='释放量未确认，请保存工作后自行重启'}}
    if($Result.Operation -eq 'analyze'){return [pscustomobject]@{Top='组件分析结束';Large='报告已生成';Note='这一步只做分析，没有执行清理'}}
    [pscustomobject]@{Top='系统命令已结束';Large='释放量未确认';Note='命令正常退出；不据此声称清理成功'}
}
function Task-Arguments($ScriptPath) {return ('-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+$ScriptPath+'" -Mode AutoTemp')}
function Change-Schedule($ScriptPath) {
    $name='CDriveStorageTool-OldTemp';$task=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    $exe="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe";$arguments=Task-Arguments $ScriptPath
    if($task){
        $actions=@($task.Actions)
        if($actions.Count -ne 1 -or $actions[0].Execute -ine $exe -or $actions[0].Arguments -cne $arguments){throw '同名任务属于其他位置，未修改'}
        Unregister-ScheduledTask -TaskName $name -Confirm:$false
        return '自动维护已关闭'
    }
    $action=New-ScheduledTaskAction -Execute $exe -Argument $arguments
    $trigger=New-ScheduledTaskTrigger -Weekly -DaysOfWeek Sunday -At '12:00'
    $principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
    $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 30) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal -Settings $settings | Out-Null
    return '自动维护已开启：周日 12:00，仅登录时执行，固定 7 天阈值'
}
function Worker-Run($Operation,$Rows,$Days,$Shared) {
    $script:Shared=$Shared
    try{
        switch($Operation){
            'migration-list'{$Shared.Result=[pscustomobject]@{Items=@(Migration-List);Drives=@(Migration-Drives)}}
            'migrate'{$r=Start-CacheMigration $Rows[0].Name $Rows[0].Drive;$Shared.Result=$r;try{$Shared.Log=Save-Record 'migration' $r}catch{$Shared.Log=Migration-StatePath $r.Name}}
            'recover'{$r=Recover-Migration $Rows[0].Name;$Shared.Result=$r;try{$Shared.Log=Save-Record 'migration' $r}catch{$Shared.Log=Migration-StatePath $r.Name}}
            'space'{$r=Scan-Space;$Shared.Result=$r;$Shared.Log=Save-Record 'space' $r;$Shared.Status='目录检查结束'}
            'scan'{Scan-Rows $Rows $Days;$Shared.Log=Save-Record 'audit' $Shared.Scanned.ToArray();$Shared.Status=if($Shared.Cancel){'检查已停止'}else{'检查完成'}}
            'clean'{$result=Clean-Rows $Rows;$Shared.Status=$result.Summary;$Shared.Log=$result.Log;$Shared.Result=$result}
            {$_ -in @('analyze','dism')}{$r=Run-ComponentOperation $Operation;$Shared.Result=$r;$Shared.Log=Save-Record 'dism' $r;$Shared.Status=(Component-Display $r).Note}
        }
    }catch{
        $Shared.Status='未完成：'+$_.Exception.Message;$Shared.Error=$_.Exception.Message
        try{$Shared.Log=Save-Record 'error' ([pscustomobject]@{Operation=$Operation;Error=$Shared.Error})}catch{}
    }
    finally{$Shared.Done=$true}
}
