param(
    [Parameter(Mandatory)][ValidateSet('scan','clean','migration-list','migrate','recover','space','analyze','dism','schedule-state','schedule','history')][string]$Operation,
    [Parameter(Mandatory)][string]$SessionPath
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
. (Join-Path $PSScriptRoot 'StorageWorker.ps1')
$SessionPath=[IO.Path]::GetFullPath($SessionPath)
$allowed=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\work\storage-dashboard\scratch'))+'\'
if(-not $SessionPath.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path $SessionPath -Leaf) -notmatch '^maintenance-[a-f0-9]{32}$'){throw '维护会话目录越界'}
Check-PlainPath $SessionPath
$request=Get-Content -LiteralPath (Join-Path $SessionPath 'request.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$resultPath=Join-Path $SessionPath 'result.json'
$scanPath=Join-Path $SessionPath 'scan.clixml'
function Write-Reply($Name,$Value){
    $path=Join-Path $SessionPath $Name
    if(Test-Path -LiteralPath $path){Check-PlainPath $path}
    $temp=Join-Path $SessionPath ($Name+'.writing')
    if(Test-Path -LiteralPath $temp){Check-PlainPath $temp}
    [IO.File]::WriteAllText($temp,(ConvertTo-Json -InputObject $Value -Depth 14 -Compress),[Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temp,$path,$true)
}
try {
    if($Operation -eq 'schedule-state'){
        Write-Reply 'result.json' @{Success=$true;Enabled=[bool](Get-ScheduledTask -TaskName 'CDriveStorageTool-OldTemp' -ErrorAction SilentlyContinue);Status='计划状态已读取'};exit
    }
    if($Operation -eq 'schedule'){
        if($request.Demo){throw '演练模式不修改计划任务'}
        $message=Change-Schedule (Join-Path $PSScriptRoot 'StorageTool.ps1')
        Write-Reply 'result.json' @{Success=$true;Enabled=[bool](Get-ScheduledTask -TaskName 'CDriveStorageTool-OldTemp' -ErrorAction SilentlyContinue);Status=$message};exit
    }
    if($Operation -eq 'history'){
        if($request.Demo){
            $demoRecord=Join-Path $SessionPath 'demo-log.json'
            $items=@()
            if(Test-Path -LiteralPath $demoRecord){
                Check-PlainPath $demoRecord
                $record=Get-Content -LiteralPath $demoRecord -Raw -Encoding UTF8 | ConvertFrom-Json
                $time=try{([datetime]$record.Time).ToString('MM-dd HH:mm')}catch{'本次演练'}
                $count=if($null -ne $record.Outcome){[int]$record.Outcome.Deleted}else{0}
                $items=@([pscustomobject]@{Label=($time+' · 演练删除 '+$count+' 个文件');Path=$demoRecord;Details=@(Detail-Rows $record)})
            }
            Write-Reply 'result.json' @{Success=$true;Items=$items;Status=$(if($items.Count){'本次演练记录'}else{'还没有演练记录'})};exit
        }
        $items=@(Cleanup-History | Select-Object -First 40 | ForEach-Object {[pscustomobject]@{Label=$_.Label;Path=$_.Path;Details=@(Detail-Rows $_.Record)}})
        Write-Reply 'result.json' @{Success=$true;Items=$items;Status='最近 40 条记录'};exit
    }
    $demoRoot=Join-Path $SessionPath 'fixture'
    if($request.Demo -and $Operation -notin @('scan','clean')){throw '演练模式只检查和清理演练文件'}
    if($Operation -eq 'scan'){
        if(Test-Path -LiteralPath $scanPath){Check-PlainPath $scanPath;Remove-Item -LiteralPath $scanPath}
        if($request.Demo){
            if(-not(Test-Path -LiteralPath $demoRoot)){
                [void][IO.Directory]::CreateDirectory($demoRoot)
                foreach($name in @('old.tmp','recent.tmp','locked.tmp')){
                    $path=Join-Path $demoRoot $name;[IO.File]::WriteAllBytes($path,[byte[]]::new(4096))
                    if($name -ne 'recent.tmp'){[IO.File]::SetCreationTime($path,(Get-Date).AddDays(-40));[IO.File]::SetLastWriteTime($path,(Get-Date).AddDays(-40))}
                }
            }
            Check-PlainPath $demoRoot;$rows=@(Build-Rows $demoRoot)
        }else{
            $allRows=@(Build-Rows)
            $ids=@($request.Ids | ForEach-Object {[string]$_} | Where-Object {$_})
            $rows=if($ids.Count){@($allRows | Where-Object {$_.Id -in $ids})}else{$allRows}
            if(-not $rows.Count -or @($ids | Where-Object {$_ -notin $rows.Id}).Count){throw '检查范围无效'}
        }
    }elseif($Operation -eq 'clean'){
        Check-PlainPath $scanPath
        $state=Import-Clixml -LiteralPath $scanPath
        if([bool]$state.Demo -ne [bool]$request.Demo){throw '预览与当前模式不一致，请重新检查'}
        if(([datetime]$state.Time).AddMinutes(30) -lt (Get-Date)){throw '预览已超过 30 分钟，请重新检查'}
        $requested=@($request.Ids | ForEach-Object {[string]$_} | Sort-Object -Unique)
        if(-not $requested.Count){throw '没有勾选要清理的项目'}
        $known=@($state.Rows | Where-Object {$_.Enabled -and ($_.Eligible -gt 0 -or ($_.Kind -eq 'Package' -and $_.Bytes -gt 0))})
        $unknown=@($requested | Where-Object {$_ -notin @($known.Id)})
        if($unknown.Count){throw ('勾选项目不在可处理预览中：'+($unknown -join '、'))}
        $rows=@($known | Where-Object {$_.Id -in $requested})
        if($rows.Count -ne $requested.Count){throw '部分勾选项目已失效，请重新检查'}
        # Consume the preview before execution; a second click cannot replay deletion.
        Remove-Item -LiteralPath $scanPath
    }else{$rows=@($request.Rows)}
    $days=[int]$request.Days
    if($Operation -eq 'scan' -and $days -notin @(7,14,30,90)){throw '保留时间无效'}
    $shared=[hashtable]::Synchronized(@{Status='正在准备';Result=$null;Error='';Log='';Cancel=$false;Done=$false;Scanned=[Collections.Generic.List[object]]::new()})
    $worker=[powershell]::Create();$lock=$null
    try {
        if($request.Demo -and $Operation -eq 'clean'){$lock=[IO.File]::Open((Join-Path $demoRoot 'locked.tmp'),'Open','ReadWrite','None')}
        $demoRecord=if($request.Demo){Join-Path $SessionPath 'demo-log.json'}else{$null}
        [void]$worker.AddScript('param($file,$op,$rows,$days,$shared,$demoRecord) . $file; $script:DemoRecordPath=$demoRecord; Worker-Run $op $rows $days $shared').AddArgument((Join-Path $PSScriptRoot 'StorageWorker.ps1')).AddArgument($Operation).AddArgument($rows).AddArgument($days).AddArgument($shared).AddArgument($demoRecord)
        $async=$worker.BeginInvoke()
        while(-not $async.IsCompleted){
            if(Test-Path -LiteralPath (Join-Path $SessionPath 'cancel')){$shared.Cancel=$true}
            Write-Reply 'progress.json' @{Status=$shared.Status}
            Start-Sleep -Milliseconds 250
        }
        [void]$worker.EndInvoke($async)
        if($shared.Error){throw $shared.Error}
        if($worker.HadErrors){throw $worker.Streams.Error[0]}
        $scanned=@($shared.Scanned.ToArray());$details=@();$display=$null
        if($Operation -eq 'scan'){
            if(-not $shared.Cancel){[pscustomobject]@{Time=(Get-Date).ToString('o');Demo=[bool]$request.Demo;Rows=$scanned} | Export-Clixml -LiteralPath $scanPath -Depth 12}
            $details=@(Detail-Rows $scanned)
        }elseif($Operation -eq 'clean'){$display=Cleanup-Display $shared.Result}
        elseif($Operation -in @('analyze','dism')){$display=Component-Display $shared.Result}
        if($Operation -ne 'scan' -and $shared.Log -and (Test-Path -LiteralPath $shared.Log)){$details=@(Detail-Rows (Get-Content -LiteralPath $shared.Log -Raw -Encoding UTF8 | ConvertFrom-Json))}
        $summary=@($scanned | Select-Object Id,Name,Kind,Bytes,Eligible,Count,State,Enabled,Note,
            @{Name='Targets';Expression={@($_.Targets | Select-Object Name,Path,Kind)}},
            @{Name='Files';Expression={@($_.Scans | ForEach-Object {$_.Files} | Select-Object -First 80 | Select-Object Path,Bytes)}},
            @{Name='FileLimitReached';Expression={@($_.Scans | ForEach-Object {$_.Files}).Count -gt 80}})
        Write-Reply 'result.json' @{Success=$true;Status=$shared.Status;Canceled=[bool]$shared.Cancel;Rows=$summary;Details=$details;Display=$display;Result=$shared.Result;Log=$shared.Log}
    }finally{if($lock){$lock.Dispose()};$worker.Dispose()}
}catch{Write-Reply 'result.json' @{Success=$false;Error=$_.Exception.Message};exit 1}
