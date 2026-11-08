# Mole Windows 规则适配

初次适配：2026-10-29。公开说明更新：2026-11-08。规则在本项目中改写为 C++，不直接运行 Mole 脚本。

## 来源

- 仓库：https://github.com/tw93/Mole
- 固定提交：`3423ee037774fec5b20ec63b912a6df65a79675d`（Windows 分支）
- 规则来源：该提交的 `lib/clean/caches.ps1`、`lib/clean/dev.ps1`、`lib/clean/user.ps1`。
- 安全行为参考：`lib/clean/cache_guard.ps1`、`lib/core/file_ops.ps1`、`lib/core/base.ps1`。
- 许可证：GPL-3.0，全文保留于 [licenses/Mole-GPL-3.0.txt](licenses/Mole-GPL-3.0.txt)。保留上游作者 tw93/Mole 的归属。本文件记录修改与范围；不表示获得额外授权。
- 本次修改：选取明确的缓存目录，移植为原生 C++ 规则；不包含 Mole 的收费产品、品牌或界面。

## 已接入范围

缓存与临时文件共 31 组，另有 1 组通过 Windows 接口执行的系统清理，总计 32 组。规则定义位于 `src/native/rules.cpp`。这里只说明所选 Mole 路径的适配，不代表覆盖全部上游规则。

| 分类 | 分组与扫描范围 |
| --- | --- |
| 原有临时文件 | 当前用户 LocalAppData/Temp、Windows/Temp、CrashDumps、SquirrelTemp；CrashDumps 限顶层 `.dmp` |
| 浏览器 | Chrome、Edge、Brave、Vivaldi 的 Default/Profile 数字目录下 Cache/Code Cache/GPUCache；Opera Stable 的这些缓存及配置子目录 |
| Firefox | LocalAppData/Mozilla/Firefox/Profiles 下 cache2/startupCache；关联检查 AppData 对应配置的 parent.lock |
| Electron 应用 | Discord、Slack、Windsurf、Obsidian、Notion、Cursor 的 Cache/Code Cache/GPUCache |
| Teams | 旧版 Cache/GPUCache；新版固定 MSTeams 包的 EBWebView 配置缓存 |
| 编辑器 | VS Code、Insiders 的三种缓存及 CachedData/CachedExtensions/CachedExtensionVSIXs；Zed 本地与漫游 cache |
| 显卡 | DirectX/D3D/Vulkan、NVIDIA DX/GL/NV_Cache、AMD DX/GL/Vk、Intel ShaderCache |
| 开发缓存 | Yarn 全局缓存、Bun install/cache、NuGet v3-cache/plugins-cache、node-gyp/electron/TypeScript 缓存 |
| 保留原有工具维护 | pip、npm、uv 的默认缓存路径统计；实际操作调用相应工具，并先核对缓存路径 |

## 安全差异

- 全部直接文件规则要求创建时间与修改时间均超过 7 天，比上游部分立即清理的缓存规则更保守。
- 新增规则和着色器规则默认不勾选；原有包管理器仍默认不勾选。
- 只使用列明的缓存叶目录，不把整个 AppData、浏览器配置或程序安装目录当作垃圾。
- 应用进程匹配支持多个进程名；进程列表读取失败按占用处理。部分开发工具使用 Python/Node 进程作为保守保护，会多保留缓存。
- Chromium/Electron 检查配置目录及其上一级的 lockfile/SingletonLock；Firefox 检查对应漫游配置的 parent.lock。目录链接、异常锁文件和无法确认的路径均保留。
- 配置目录发现最多枚举 128 项；达到上限、权限错误、扫描超时显示不完整。扫描仍受总 30 秒、每组 4 秒、最大深度 96 的限制；目录发现使用独立的条目上限。
- 跳过 reparse point、离线/按需读取文件、硬链接和受保护虚拟磁盘/密码库扩展；临时目录保留诊断与 node-compile-cache 子目录。
- 扫描记录文件卷号、文件 ID、大小、创建/修改时间。删除前复核路径白名单和同一文件身份，并保持原有父目录固定、独占文件句柄、预览有效期与日志机制。
- 占用检查申请读取权限以触发 Windows 共享锁检查，但不读取文件内容。预演不申请 DELETE 权限、不设置删除标记。
- 当前使用 v5 预览格式；拒绝 v3/v4 旧预览执行清理，旧历史摘要仍可读取。

## 未接入的上游范围

以下没有被归入可清理候选：WER 整报告清理、Office 同步/恢复缓存、OneDrive/DriveFS 离线内容、聊天/媒体文件、下载文件夹、回收站、孤立 AppData、Docker/WSL 数据、Windows Installer/WinSxS、Maven/Gradle/Cargo 仓库、NuGet packages、模型及虚拟磁盘。

部分需要额外配置发现或所有者接口的 pnpm/Go/mise，以及 Adobe/Autodesk、游戏启动器、其他编辑器、云工具日志等规则，未接入。没有用近似路径替代它们。Chrome 根目录 shader-cache 和 Firefox 漫游 shader-cache 也未纳入，以保持缓存与配置数据的边界。

## 验证入口

先构建：`work/Build-NativeStorage.ps1`。以下命令均针对构建结果，只有扫描和元数据预演：

```powershell
.\scratch\native-build\StorageWorker.exe --scan-rule-test <测试报告绝对路径>
.\scratch\native-build\StorageWorker.exe --audit --data-path <报告目录绝对路径>
.\scratch\native-build\StorageDashboard.exe --profile-readonly <界面验证报告绝对路径>
```

不要用 `--self-test` 或 `--hidden-ui-test` 代替上述入口：旧测试包含清理模拟文件。只需检查规则时，不必运行这两个入口，也不要运行 `--auto-temp`、包缓存清理或 DISM。

`--audit` 输出每组统计、实际存在的根路径、保留原因及 `.candidates.tsv` 全量候选清单。清单中的预演结果 0 表示当时通过检查；非 0 表示保留。它不是释放空间承诺，也不验证将来的真实删除权限。

本机没有安装或没有产生缓存的新增应用，只通过隔离样本验证规则发现，不能据此宣称已对每款真实应用完成验证。公开整理阶段仅做构建与隔离规则测试，不执行真实清理。
