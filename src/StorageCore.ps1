function Size-Text([long]$Bytes) {
    if($Bytes -ge 1GB){return ('{0:N2} GiB' -f ($Bytes/1GB))}
    if($Bytes -ge 1MB){return ('{0:N1} MiB' -f ($Bytes/1MB))}
    if($Bytes -ge 1KB){return ('{0:N1} KiB' -f ($Bytes/1KB))}
    if($Bytes -gt 0){return ($Bytes.ToString()+' B')}
    return '0 B'
}

function Free-Bytes { return [long](Get-PSDrive -Name C).Free }

function Is-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Check-PlainPath([string]$Path) {
    $item=Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    while($null -ne $item){
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw '路径含符号链接或目录联接，已跳过。'}
        if($item.PSIsContainer){$item=$item.Parent}else{$item=$item.Directory}
    }
}

function Target($Name,$Path,$Kind,$Process='') {
    [pscustomobject]@{Name=$Name;Path=$Path;Kind=$Kind;Process=$Process}
}

function Temp-Targets {
    Target '用户临时文件' (Join-Path $env:LOCALAPPDATA 'Temp') 'Temp'
    Target 'Windows 临时文件' (Join-Path $env:SystemRoot 'Temp') 'Temp'
    Target '旧崩溃转储' (Join-Path $env:LOCALAPPDATA 'CrashDumps') 'Temp'
    Target 'Squirrel 更新残留' (Join-Path $env:LOCALAPPDATA 'SquirrelTemp') 'Temp'
}

function Browser-Targets([string]$ProcessFilter='') {
    foreach($browser in @(@('Chrome','Google\Chrome\User Data','chrome'),@('Edge','Microsoft\Edge\User Data','msedge'))){
        if($ProcessFilter -and $browser[2] -ne $ProcessFilter){continue}
        $base=Join-Path $env:LOCALAPPDATA $browser[1]
        if(-not(Test-Path -LiteralPath $base)){continue}
        Check-PlainPath $base
        foreach($profile in Get-ChildItem -LiteralPath $base -Directory -Force -ErrorAction Stop){
            if($profile.Name -notmatch '^(Default|Profile \d+)$'){continue}
            foreach($cache in @('Cache','Code Cache','GPUCache')){
                Target ($browser[0]+' / '+$profile.Name+' / '+$cache) (Join-Path $profile.FullName $cache) 'Browser' $browser[2]
            }
        }
    }
    foreach($app in @(@('VS Code','Code','Code'),@('Cursor','Cursor','Cursor'))){
        if($ProcessFilter -and $app[2] -ne $ProcessFilter){continue}
        foreach($cache in @('Cache','Code Cache','GPUCache')){
            Target ($app[0]+' / '+$cache) (Join-Path (Join-Path $env:APPDATA $app[1]) $cache) 'Browser' $app[2]
        }
    }
}

function Other-Targets {
    Target 'pip 下载缓存' (Join-Path $env:LOCALAPPDATA 'pip\cache') 'Package'
    Target 'npm 内容缓存' (Join-Path $env:LOCALAPPDATA 'npm-cache\_cacache') 'Package'
    Target 'uv 缓存' (Join-Path $env:LOCALAPPDATA 'uv\cache') 'Package'
    Target 'Go 构建缓存' (Join-Path $env:LOCALAPPDATA 'go-build') 'Guide'
    Target 'DirectX 着色器缓存' (Join-Path $env:LOCALAPPDATA 'D3DSCache') 'Official'
    Target 'NVIDIA 本地数据' (Join-Path $env:LOCALAPPDATA 'NVIDIA') 'Guide'
    Target 'Docker 本地数据' (Join-Path $env:LOCALAPPDATA 'Docker') 'Migrate'
    Target 'WSL 默认数据目录' (Join-Path $env:LOCALAPPDATA 'wsl') 'Migrate'
}

function Scan-Target($Target,[int]$Days=7,[int]$BudgetMs=8000) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$limited=$false
    $files=[Collections.Generic.List[object]]::new();$issues=[Collections.Generic.List[object]]::new();$metadata=[Collections.Generic.List[string]]::new()
    [long]$total=0;[long]$eligible=0;$skipped=0;$count=0
    $root=[IO.Path]::GetFullPath($Target.Path).TrimEnd('\')
    try{[void](Get-Item -LiteralPath $root -Force -ErrorAction Stop)}catch{
        if($_.CategoryInfo.Category -eq 'ObjectNotFound'){return}
        throw
    }
    try{Check-PlainPath $root}catch{
        return [pscustomobject]@{Target=$Target;Bytes=0;Eligible=0;Count=0;Skipped=1;Files=@();Issues=@([pscustomobject]@{Path=$root;Reason=(Reason-Text $_.Exception.Message)});Fingerprint='';Limited=$false;Note=$_.Exception.Message}
    }
    $cutoff=(Get-Date).AddDays(-$Days)
    $stack=[Collections.Generic.Stack[string]]::new();$stack.Push($root)
    while($stack.Count -gt 0){
        if(Test-Cancel){throw '用户已停止扫描'}
        if($clock.ElapsedMilliseconds -ge $BudgetMs){$limited=$true;break}
        $dir=$stack.Pop()
        try{Check-PlainPath $dir;$entries=Get-ChildItem -LiteralPath $dir -Force -ErrorAction Stop}catch{$skipped++;$issues.Add([pscustomobject]@{Path=$dir;Reason=(Reason-Text $_.Exception.Message)});continue}
        foreach($entry in $entries){
            if(Test-Cancel){throw '用户已停止扫描'}
            if($clock.ElapsedMilliseconds -ge $BudgetMs){$limited=$true;break}
            if($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){$skipped++;continue}
            if($entry.PSIsContainer){
                if($Target.Kind -eq 'Temp' -and $entry.Name -match 'DiagOutput|diagnostic|node-compile-cache'){$skipped++;continue}
                $stack.Push($entry.FullName);continue
            }
            $total+=$entry.Length;$count++
            if($Target.Kind -eq 'Package'){$metadata.Add($entry.FullName+'|'+$entry.Length+'|'+$entry.LastWriteTimeUtc.Ticks+'|'+$entry.CreationTimeUtc.Ticks)}
            $can=($Target.Kind -in @('Temp','Shader') -and $entry.LastWriteTime -lt $cutoff -and $entry.CreationTime -lt $cutoff -and $entry.Extension -notin @('.vhd','.vhdx','.vmdk','.kdbx'))
            if($Target.Kind -eq 'Browser'){$can=$true}
            if($can -and $entry.Length -gt 0){
                # Read-only lock probe. This is not a guarantee of delete permission.
                try{$probe=[IO.File]::Open($entry.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None);$probe.Dispose()}
                catch{$issues.Add([pscustomobject]@{Path=$entry.FullName;Reason=(Reason-Text $_.Exception.Message)});continue}
                $eligible+=$entry.Length
                $files.Add([pscustomobject]@{Path=$entry.FullName;Root=$root;Bytes=$entry.Length;Modified=$entry.LastWriteTimeUtc.Ticks;Created=$entry.CreationTimeUtc.Ticks})
            }
        }
        if($limited){break}
        Report-State ('正在扫描 '+$Target.Name+' · '+$count+' 个文件')
        if(Test-Cancel){throw '用户已停止扫描'}
    }
    if($limited){$skipped++;$issues.Add([pscustomobject]@{Path=$root;Reason='达到检查时限，剩余内容未检查'})}

    $fingerprint=''
    if($Target.Kind -eq 'Package'){
        $sha=[Security.Cryptography.SHA256]::Create()
        try{$fingerprint=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes((@($metadata | Sort-Object) -join "`n"))))}finally{$sha.Dispose()}
    }
    [pscustomobject]@{Target=$Target;Bytes=$total;Eligible=$eligible;Count=$count;Skipped=$skipped;Files=$files.ToArray();Issues=$issues.ToArray();Fingerprint=$fingerprint;Limited=$limited;Note=''}
}

function Delete-Candidates($Scan,$AllowedTargets) {
    $allowed=@($AllowedTargets | ForEach-Object{[IO.Path]::GetFullPath($_.Path).TrimEnd('\')})
    foreach($candidate in $Scan.Files){
        if(Test-Cancel){break}
        $status='Skipped';$reason=''
        try{
            $path=[IO.Path]::GetFullPath($candidate.Path)
            $root=[IO.Path]::GetFullPath($candidate.Root).TrimEnd('\')
            if($root -notin $allowed -or -not $path.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw '超出允许范围'}
            if($Scan.Target.Process -and (Get-Process -Name $Scan.Target.Process -ErrorAction SilentlyContinue)){throw '应用已启动，跳过缓存'}
            Check-PlainPath $path
            $item=Get-Item -LiteralPath $path -Force -ErrorAction Stop
            if($item.PSIsContainer -or $item.Length -ne $candidate.Bytes -or $item.LastWriteTimeUtc.Ticks -ne $candidate.Modified -or $item.CreationTimeUtc.Ticks -ne $candidate.Created){throw '文件在预览后发生变化'}
            Remove-Item -LiteralPath $path -Force -ErrorAction Stop
            # Confirm absence separately; command completion alone is not the result.
            try{
                [void](Get-Item -LiteralPath $path -Force -ErrorAction Stop)
                $status='Unconfirmed';$reason='删除后路径仍存在或已被重建，未计入删除量'
            }catch{
                if($_.CategoryInfo.Category -eq 'ObjectNotFound'){$status='Deleted'}
                else{$status='Unconfirmed';$reason='删除后无法核实：'+$_.Exception.Message}
            }
        }catch{$reason=Reason-Text $_.Exception.Message}
        [pscustomobject]@{Group=$Scan.Target.Name;Path=$candidate.Path;Bytes=$candidate.Bytes;Status=$status;Reason=$reason}
    }
}

function Self-Test {
    $sandbox=Join-Path $PSScriptRoot ('test-'+[guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $sandbox)
    try{
        $old=Join-Path $sandbox 'old.tmp';$new=Join-Path $sandbox 'new.tmp';$changed=Join-Path $sandbox 'changed.tmp'
        $outside=Join-Path $sandbox 'outside';[void](New-Item -ItemType Directory -Path $outside)
        foreach($p in @($old,$new,$changed)){[IO.File]::WriteAllText($p,'fixture')}
        foreach($p in @($old,$changed)){[IO.File]::SetCreationTime($p,(Get-Date).AddDays(-10));[IO.File]::SetLastWriteTime($p,(Get-Date).AddDays(-10))}
        $t=Target 'test' $sandbox 'Temp';$scan=Scan-Target $t 7
        if($scan.Files.Count -ne 2){throw 'FAIL: age selection'}
        [IO.File]::WriteAllText($changed,'changed after preview')
        $result=@(Delete-Candidates $scan @($t))
        if((Test-Path -LiteralPath $old) -or -not(Test-Path -LiteralPath $new) -or -not(Test-Path -LiteralPath $changed)){throw 'FAIL: delete or change guard'}
        if(@($result | Where-Object Status -eq 'Skipped').Count -ne 1){throw 'FAIL: expected changed-file skip'}
        $escape=[pscustomobject]@{Target=$t;Files=@([pscustomobject]@{Root=$outside;Path=$new;Bytes=7;Modified=0;Created=0})}
        $r=@(Delete-Candidates $escape @($t));if($r[0].Status -ne 'Skipped'){throw 'FAIL: allowlist'}
        Say 'PASS: age cutoff, preview-change guard, scoped deletion, allowlist escape guard.' Green
    }finally{
        $expected=[IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\test-'
        $resolved=[IO.Path]::GetFullPath($sandbox)
        if(-not $resolved.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase)){throw 'Test cleanup path mismatch'}
        Check-PlainPath $sandbox
        Remove-Item -LiteralPath $sandbox -Recurse -Force
    }
}
