function Shader-Targets {
    Target 'DirectX 着色器' (Join-Path $env:LOCALAPPDATA 'D3DSCache') 'Shader'
    Target 'NVIDIA DirectX 着色器' (Join-Path $env:LOCALAPPDATA 'NVIDIA\DXCache') 'Shader'
    Target 'NVIDIA OpenGL 着色器' (Join-Path $env:LOCALAPPDATA 'NVIDIA\GLCache') 'Shader'
}
function Reason-Text($ErrorRecord) {
    $message=[string]$ErrorRecord
    if($message -match 'being used|used by another|正.*使用|占用|应用已启动'){return '正在使用，已保留'}
    if($message -match 'denied|拒绝|权限'){return '权限不足，已保留'}
    if($message -match '发生变化'){return '检查后已变化，已保留'}
    if($message -match '链接|联接'){return '链接目录，已保留'}
    return $message
}
function Cache-Fingerprint($Scans) {
    $parts=@($Scans | ForEach-Object {$_.Fingerprint})
    return ($parts -join ':')
}
function Get-MaintenanceState {
    $path=Join-Path $PSScriptRoot 'logs\maintenance-state.json'
    try {if(Test-Path -LiteralPath $path){$value=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json;foreach($entry in $value){$entry};return}}catch{}
    return @()
}
function Save-NoChange($Id,$Info,$Before,$After,$ExitCode) {
    $entries=@(Get-MaintenanceState | Where-Object Id -ne $Id)
    if($ExitCode -eq 0 -and $Before -and $After -and $Before.Complete -and $After.Complete -and $Before.Fingerprint -eq $After.Fingerprint){
        $entries+= [pscustomobject]@{Id=$Id;Path=$Info.Measure;Fingerprint=$After.Fingerprint;Time=(Get-Date).ToString('o')}
    }
    try{
        $dir=Join-Path $PSScriptRoot 'logs';[void](New-Item -ItemType Directory -Path $dir -Force)
        ConvertTo-Json -InputObject @($entries) -Depth 4 | Set-Content -LiteralPath (Join-Path $dir 'maintenance-state.json') -Encoding UTF8
    }catch{} # Recommendation history must not change the cleanup result.
}
function Measure-PackageState($Info) {
    $s=Scan-Target (Target $Info.Name $Info.Measure 'Package') 7
    if(-not $s){return [pscustomobject]@{Bytes=0L;Fingerprint='missing';Complete=$true}}
    [pscustomobject]@{Bytes=[long]$s.Bytes;Fingerprint=$s.Fingerprint;Complete=($s.Skipped -eq 0)}
}
function Detail-Rows($Record) {
    if($Record -isnot [array] -and $Record.Operation -eq 'migration'){
        $state=switch($Record.Phase){'Complete'{'迁移完成'} 'RolledBack'{'已恢复原位'} default{'需要恢复'}}
        [pscustomobject]@{Name=($Record.Name+' 原占用');Size=(Size-Text $Record.SourceBytes);State=$state;Note=('源：'+$Record.Source+"`r`n目标："+$Record.Destination+"`r`n"+$Record.Error)}
        [pscustomobject]@{Name='内容校验';Size=(Size-Text $Record.VerifiedBytes);State='逐文件 SHA256';Note='核对相对路径、文件数量、长度和 SHA256；并由新进程读取持久配置，验证目标读写。'}
        [pscustomobject]@{Name='移除旧文件';Size=(Size-Text $Record.RemovedSourceBytes);State='文件大小';Note='回滚后此值归零。逻辑文件大小不等于磁盘实际释放量。'}
        [pscustomobject]@{Name='磁盘释放';Size='未知';State='不作推算';Note=$(if($Record.TargetRetained){'目标副本仍保留：'+$Record.Destination}else{'配置与文件均记录在本机迁移日志。'})}
    }elseif($Record -isnot [array] -and $Record.Outcome){
        foreach($group in @($Record.Files | Where-Object {$_.Status} | Group-Object Group)){
            $ok=@($group.Group | Where-Object Status -eq 'Deleted')
            $bad=@($group.Group | Where-Object Status -ne 'Deleted')
            $reasons=@($bad | Group-Object Reason | ForEach-Object {('{0} × {1}' -f (Reason-Text $_.Name),$_.Count)})
            [pscustomobject]@{Name=$group.Name;Size=(Size-Text ([long](($ok|Measure-Object Bytes -Sum).Sum)));State=('删除 '+$ok.Count+' · 保留 '+$bad.Count);Note=($reasons -join "`r`n")}
        }
        foreach($p in @($Record.Files | Where-Object {$_.Item})){
            $state=if($null -eq $p.ReducedBytes){'未确认'}elseif($p.ReducedBytes -gt 0){'目录减少'}else{'没有减少'}
            if($p.ExitCode -ne 0 -or $p.Error){$state='命令失败；'+$state}
            $size=if($null -eq $p.ReducedBytes){'未知'}else{Size-Text $p.ReducedBytes}
            $note=if($null -ne $p.BeforeBytes -and $null -ne $p.AfterBytes){'占用 '+(Size-Text $p.BeforeBytes)+' → '+(Size-Text $p.AfterBytes)}else{'大小未完整读取'}
            [pscustomobject]@{Name=$p.Item;Size=$size;State=$state;Note=$note}
        }
    }elseif($Record -isnot [array] -and $Record.Operation -and -not $Record.Error){
        $v=Component-Display $Record
        [pscustomobject]@{Name='Windows 组件';Size='未知';State=$v.Large;Note=$v.Note}
    }elseif($Record -isnot [array] -and $Record.Error){
        [pscustomobject]@{Name='操作未完成';Size='—';State='请重试';Note=(Reason-Text $Record.Error)}
    }else{
        foreach($r in @($Record)){
            $occupancy=Size-Text $r.Bytes
            if(@($r.Scans | Where-Object {$_.Skipped -gt 0}).Count){$occupancy='至少 '+$occupancy}
            $candidate=if($r.Kind -eq 'Package'){'可处理量未知；由官方工具判断'}else{'待处理 '+(Size-Text $r.Eligible)+'（文件大小）'}
            if($r.State -eq '近期无变化'){$candidate='实测与上次无变化；7 天内不重复推荐'}
            if($r.State -eq '使用中'){$candidate='应用运行中；关闭后重新检查'}
            if($r.State -eq '不可用'){$candidate=$r.Note}
            $issues=@($r.Scans | ForEach-Object {$_.Issues} | Group-Object Reason | ForEach-Object {('{0} × {1}' -f $_.Name,$_.Count)})
            [pscustomobject]@{Name=$r.Name;Size=$occupancy;State=$r.State;Note=($candidate+ $(if(@($r.Scans|Where-Object {$_.Skipped -gt 0}).Count){"`r`n部分目录保留或无法读取"})+ $(if($issues){"`r`n"+($issues -join "`r`n")}))}
        }
    }
}

function Drive-Status {
    try{
        $drive=[IO.DriveInfo]::new('C:\')
        [pscustomobject]@{Time=(Get-Date).ToString('o');TotalBytes=$drive.TotalSize;AvailableBytes=$drive.AvailableFreeSpace;Text=('C 盘可用 '+(Size-Text $drive.AvailableFreeSpace))}
    }catch{[pscustomobject]@{Time=(Get-Date).ToString('o');TotalBytes=$null;AvailableBytes=$null;Text='C 盘可用空间未读取'}}
}
function Space-GuideUri($Row) {
    # Resolve by the source path, not translated display text.
    if($Row.Path -ieq (Join-Path $env:LOCALAPPDATA 'Docker')){return 'https://docs.docker.com/desktop/features/wsl/'}
    return 'https://learn.microsoft.com/en-us/windows/wsl/basic-commands#export-a-distribution'
}
function Cleanup-History {
    $dir=Join-Path $PSScriptRoot 'logs'
    if(-not(Test-Path -LiteralPath $dir)){return}
    foreach($f in @(Get-ChildItem -LiteralPath $dir -File | Where-Object {$_.Name -match '-(cleanup|migration)\.json$'} | Sort-Object Name -Descending | Select-Object -First 20)){
        try{
            if($f.Length -gt 8MB){continue}
            $r=Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json
            if($r.Operation -eq 'migration'){
                [pscustomobject]@{Label=(([datetime]$r.Time).ToString('MM-dd HH:mm')+' · '+$r.Name+' 迁移');Path=$f.FullName;Record=$r;IsDemo=$false};continue
            }
            if(-not $r.Outcome){continue}
            $demo=@($r.Files | Where-Object {$_.Group -match '演练|^test$' -or $_.Path -match '\\(demo|test)-[^\\]+\\'}).Count -gt 0
            $kind=if($demo){'演练'}elseif($r.Outcome.Canceled){'已停止'}elseif($r.Outcome.Deleted -gt 0){'文件'}elseif($r.Outcome.PackageReducedBytes -gt 0){'缓存'}else{'未减少'}
            $label=([datetime]$r.Time).ToString('MM-dd HH:mm')+' · '+$kind
            [pscustomobject]@{Label=$label;Path=$f.FullName;Record=$r;IsDemo=$demo}
        }catch{continue}
    }
}
function Space-Targets {
    # Common locations only, never a claim to have measured the entire drive.
    $roots=[Collections.Generic.List[object]]::new()
    foreach($base in @($env:LOCALAPPDATA,$env:APPDATA,$env:ProgramFiles,${env:ProgramFiles(x86)},$env:ProgramData)){
        if(-not $base -or -not $base.StartsWith('C:\',[StringComparison]::OrdinalIgnoreCase)){continue}
        try{
            Check-PlainPath $base
            foreach($dir in Get-ChildItem -LiteralPath $base -Directory -Force -ErrorAction Stop){
                if($dir.Attributes -band [IO.FileAttributes]::ReparsePoint){continue}
                $action=if($base -in @($env:ProgramFiles,${env:ProgramFiles(x86)})){'卸载应用'}else{'打开位置'}
                $note='仅显示文件大小；请通过应用设置迁移或卸载。'
                if($base -in @($env:LOCALAPPDATA,$env:APPDATA) -and $dir.Name -in @('uv','pip','npm-cache','NVIDIA','D3DSCache','Temp','Google','Code','Cursor')){$action='检查缓存'}
                if($base -eq $env:LOCALAPPDATA -and $dir.Name -in @('Docker','wsl')){$action='迁移说明';$note='虚拟磁盘不可直接删除。通过 Docker 设置或 WSL 导出／导入迁移。'}
                $roots.Add([pscustomobject]@{Name=$(if($action -eq '迁移说明'){$dir.Name+' 数据'}else{$dir.Name});Path=$dir.FullName;Action=$action;Note=$note})
            }
        }catch{}
    }
    foreach($name in @('.cache','.ollama','.gradle','.nuget','.cargo','.rustup','.android','.vscode')){
        $path=Join-Path $env:USERPROFILE $name
        if($path.StartsWith('C:\',[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $path)){$roots.Add([pscustomobject]@{Name=$(if($name -eq '.android'){'Android 数据'}else{$name});Path=$path;Action='打开位置';Note='开发数据或模型，只查看；不作为清理候选。'})}
    }
    $userFolders=@([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('MyDocuments'),[Environment]::GetFolderPath('MyVideos'))
    try{$userFolders+=Get-ItemPropertyValue 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' '{374DE290-123F-4565-9164-39C4925E467B}' -ErrorAction Stop}catch{}
    foreach($path in $userFolders){
        $path=[Environment]::ExpandEnvironmentVariables([string]$path)
        if($path -and $path.StartsWith('C:\',[StringComparison]::OrdinalIgnoreCase)){$roots.Add([pscustomobject]@{Name=(Split-Path $path -Leaf);Path=$path;Action='打开位置';Note='个人文件，只查看，不自动清理。'})}
    }
    # Native WSL locations can be outside AppData; registry values are paths only.
    try{foreach($d in Get-ChildItem 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss' -ErrorAction Stop){
        $p=Get-ItemProperty $d.PSPath;$path=[string]$p.BasePath
        if($path.StartsWith('C:\',[StringComparison]::OrdinalIgnoreCase)){$roots.Add([pscustomobject]@{Name=('WSL · '+$p.DistributionName);Path=$path;Action='迁移说明';Note='使用 WSL 导出／导入迁移；不直接搬动虚拟磁盘。'})}
    }}catch{}
    $seen=[Collections.Generic.List[string]]::new()
    foreach($r in @($roots | Sort-Object @{e={$_.Path.Length}},Path)){
        $path=[IO.Path]::GetFullPath($r.Path).TrimEnd('\')
        if(@($seen | Where-Object {$path -ieq $_ -or $path.StartsWith($_+'\',[StringComparison]::OrdinalIgnoreCase)}).Count){continue}
        $seen.Add($path);$r
    }
}
function Scan-Space {
    if(-not ('StorageMeasure' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'StorageMeasure.cs')}
    $clock=[Diagnostics.Stopwatch]::StartNew();$records=[Collections.Generic.List[object]]::new()
    $targets=@(Space-Targets | Sort-Object @{e={if($_.Name -match 'Docker|wsl|Downloads|下载|NVIDIA|uv|hugging|ollama'){0}else{1}}},Name)
    foreach($t in $targets){
        if(Test-Cancel){break}
        if($clock.Elapsed.TotalSeconds -ge 25){break}
        Report-State ('正在查看 '+$t.Name)
        $m=[StorageMeasure]::Read($t.Path,1500)
        $records.Add([pscustomobject]@{Name=$t.Name;Path=$t.Path;Action=$t.Action;Note=$t.Note;Bytes=$m.Bytes;Complete=$m.Complete;Files=$m.Files;Skipped=$m.Skipped;CheckedAt=(Get-Date).ToString('o');EligibleBytes=$null;ReclaimableBytes=$null})
    }
    [pscustomobject]@{Time=(Get-Date).ToString('o');Scope='常见目录，不是全盘扫描';Seconds=$clock.Elapsed.TotalSeconds;TargetCount=$targets.Count;CheckedCount=$records.Count;Canceled=[bool](Test-Cancel);Rows=@($records.ToArray() | Sort-Object Bytes -Descending);SizeBasis='LogicalFileBytes';ReclaimableBytes=$null}
}
