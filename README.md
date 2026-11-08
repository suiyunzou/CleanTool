# CleanTool

一个轻量的 Windows 磁盘清理工具。先扫描，再查看和勾选，最后确认清理。

界面使用 C++ / Win32 编写。扫描和清理由独立进程执行，界面可以显示进度、停止任务，并查看本地操作记录。

## 下载与使用

1. 打开 [Releases](https://github.com/suiyunzou/CleanTool/releases)，下载 `CleanTool-windows-x64.zip`，解压到可写的文件夹。
2. 双击 `启动.cmd`。Windows 会请求管理员权限，因为程序包含系统清理功能。
3. 点击 **开始扫描**。扫描结束后，查看每类项目的大小和状态；将鼠标移到项目上可查看说明。
4. 只勾选自己需要清理的项目，再点击底部清理按钮。所有可用项都被选中时，按钮可能显示“选择全部”；以弹出的确认内容为准。
5. 核对确认框中的项目后再执行。需要时可停止任务；已经删除的文件不会因此恢复。

请保留压缩包里的目录结构。`StorageDashboard.exe` 和 `StorageWorker.exe` 必须配套使用。GitHub 的 **Code → Download ZIP** 下载的是源码，不含编译好的程序。

目前已在 **Windows 10 x64** 本机验证。Windows 11、ARM64 和其他电脑配置尚未完成验证。运行原生程序不需要另装 Python、Node.js 或 .NET；清理 pip、npm、uv 缓存时，需要相应工具已安装并可从 PATH 找到。

## 能清理什么

目前共有 32 个分组，具体目录见 [规则源码](src/native/rules.cpp)。

| 类型 | 主要范围 |
| --- | --- |
| 临时文件 | 用户和 Windows 临时目录、旧崩溃转储、Squirrel 更新残留 |
| 浏览器缓存 | Chrome、Edge、Firefox、Brave、Vivaldi、Opera 的指定缓存目录 |
| 应用和编辑器缓存 | VS Code、Cursor、Discord、Slack、Teams、Windsurf、Obsidian、Notion、Zed 等 |
| 显卡缓存 | DirectX、NVIDIA、AMD、Intel 的指定着色器缓存 |
| 开发缓存 | Yarn、Bun、NuGet 下载缓存和部分 Node 构建工具缓存；pip、npm、uv 调用各自的维护命令 |
| Windows 系统清理 | 通过 Windows 提供的清理接口处理更新清理、传递优化缓存和临时安装文件 |

“应用在列表里”表示存在对应规则，不表示已在每个应用版本上实测。程序只检查列出的目录；自定义缓存位置可能无法识别。

## 清理前要知道

- 直接删除缓存文件时，通常要求创建时间和修改时间都超过 7 天。pip、npm、uv 维护和 Windows 系统清理由各自工具决定范围，不适用这条时间限制。
- 正在使用、已变化、路径不明确、目录链接、硬链接以及部分受保护文件会被跳过。跳过不等于程序出错。
- “不完整”表示有目录无法读取或达到扫描限制，结果可能少于实际占用。
- **清理会直接删除文件，不经过回收站，也没有一键撤销。** 缓存之后可能需要重新下载或生成。初次使用建议少量勾选。
- 主界面的清理规则不包含个人下载文件夹、文档、回收站、浏览器书签和密码，也不把整个 AppData 当作垃圾目录。
- 显示的缓存大小、删除文件大小、磁盘空闲空间变化是不同数值。其他程序也会读写磁盘，不能把它们视为相同的释放量。

Windows 系统清理使用微软的 [IEmptyVolumeCache 接口](https://learn.microsoft.com/en-us/windows/win32/api/emptyvc/nn-emptyvc-iemptyvolumecache)。仓库也保留了 Windows 组件维护代码；组件清理的行为可查阅 [微软说明](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/clean-up-the-winsxs-folder)。不要手动删除 WinSxS 文件。

## 隐私与本地记录

主程序没有账号登录、遥测上传或自动更新功能。扫描候选列表和操作记录保存在项目的 `scratch/`、`logs/` 中；测试和审计报告可能保存在 `outputs/`。这些文件可能包含用户名、文件路径和软件使用信息。

这三个目录均被 Git 忽略。公开源码不包含个人扫描记录、历史测试截图、本机构建产物或原有私人提交历史。提交 Issue 或截图前，请遮住用户名、路径和其他个人信息。调用的第三方工具仍遵循各自的行为和配置。

## 从源码编译

需要 Windows x64、PowerShell 7，以及安装了 **使用 C++ 的桌面开发** 组件的 Visual Studio / Build Tools（MSVC 与 Windows SDK）。安装和命令行环境见 [微软构建工具文档](https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line)。

在项目根目录打开 PowerShell：

```powershell
pwsh -NoProfile -File .\work\Build-NativeStorage.ps1
```

脚本会自动寻找 MSVC 和 Windows SDK。生成的两个程序位于 `scratch/native-build/`。如要通过根目录的 `启动.cmd` 运行，关闭已打开的 CleanTool 后执行：

```powershell
Copy-Item .\scratch\native-build\StorageDashboard.exe .\src\StorageDashboard.exe
Copy-Item .\scratch\native-build\StorageWorker.exe .\src\StorageWorker.exe
.\启动.cmd
```

如果使用自定义工具链，可向构建脚本传入 `-MsvcPath`、`-WindowsSdkPath`、`-WindowsSdkVersion`。其中 `MsvcPath` 指向含 `bin`、`include` 和 `lib` 的 MSVC 版本目录。程序按 x64 编译；构建本身不清理系统文件。

### 验证方法

规则测试使用独立样本目录，不扫描真实应用缓存。它会将样本留在 `scratch/sessions/`，便于复核。以下入口通过工作进程执行，无需启动管理员界面：

```powershell
$report = Join-Path $PWD 'scratch\rule-check.txt'
$test = Start-Process .\scratch\native-build\StorageWorker.exe `
    -ArgumentList @('--scan-rule-test', ('"' + $report + '"')) `
    -WindowStyle Hidden -Wait -PassThru
Get-Content -LiteralPath $report
if ($test.ExitCode -ne 0) { throw '规则测试失败' }
```

需要检查删除边界和隐藏界面时，可运行 `work/Test-NativeStorage.ps1`。它会创建并删除隔离样本，报告写入 `outputs/`；主程序需要管理员权限。可选的 `-RealScan` 会读取真实目录，不执行真实清理，生成的报告仍应保留在本地。

## 项目结构

| 路径 | 用途 |
| --- | --- |
| `src/native/` | 当前主程序、扫描规则、清理逻辑和内置测试 |
| `src/assets/` | 程序图标 |
| `src/*.ps1`、`src/*.cs` | 兼容入口及早期维护、迁移代码；当前原生界面不提供全部旧功能 |
| `work/Build-NativeStorage.ps1` | x64 构建入口 |
| `work/Test-NativeStorage.ps1` | 隔离样本和隐藏界面测试入口 |
| `work/mole-windows-rules.md` | Mole 规则的来源、改动和范围 |
| `THIRD_PARTY_NOTICES.md` | 第三方归属和引用 |
| `logs/`、`outputs/`、`scratch/` | 本地记录、报告和临时文件，不提交 |

```mermaid
flowchart LR
    A[启动.cmd] --> B[主界面]
    B --> C[独立工作进程]
    C --> D[扫描指定缓存目录]
    D --> E[查看结果并勾选]
    E --> F[确认清理]
    F --> G[重新检查文件后执行]
    G --> H[保存本地操作记录]
```

## 来源与许可证

部分 Windows 缓存路径和保护思路改编自 **[tw93/Mole](https://github.com/tw93/Mole)**，固定参考提交为 [`3423ee0`](https://github.com/tw93/Mole/tree/3423ee037774fec5b20ec63b912a6df65a79675d)。本项目将其中部分 PowerShell 规则改写为 C++，并调整了时间限制、路径检查和扫描范围。它不是 Mole 官方 Windows 发行版。

本项目以 **GNU GPL v3** 发布，见 [LICENSE](LICENSE)。上游许可证副本、具体文件链接及其他技术资料见 [第三方声明](THIRD_PARTY_NOTICES.md)。软件按许可证中的“现状”条款提供；提交问题时请附上复现步骤，并去掉个人数据。
