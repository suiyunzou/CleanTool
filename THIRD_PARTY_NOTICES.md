# 第三方来源与归属

## tw93/Mole

- 项目与作者归属：[tw93/Mole](https://github.com/tw93/Mole)，tw93 与 Mole contributors。
- 固定参考版本：[`3423ee037774fec5b20ec63b912a6df65a79675d`](https://github.com/tw93/Mole/tree/3423ee037774fec5b20ec63b912a6df65a79675d)。以下链接都固定到此提交。
- 上游许可证：[GNU GPL v3 原文](https://github.com/tw93/Mole/blob/3423ee037774fec5b20ec63b912a6df65a79675d/LICENSE)。本地完整副本：[work/licenses/Mole-GPL-3.0.txt](work/licenses/Mole-GPL-3.0.txt)。

| 上游文件 | 本项目采用的内容 |
| --- | --- |
| [lib/clean/caches.ps1](https://github.com/tw93/Mole/blob/3423ee037774fec5b20ec63b912a6df65a79675d/lib/clean/caches.ps1) | 部分浏览器、应用和显卡缓存目录 |
| [lib/clean/dev.ps1](https://github.com/tw93/Mole/blob/3423ee037774fec5b20ec63b912a6df65a79675d/lib/clean/dev.ps1) | 部分开发工具缓存目录 |
| [lib/clean/user.ps1](https://github.com/tw93/Mole/blob/3423ee037774fec5b20ec63b912a6df65a79675d/lib/clean/user.ps1) | 部分用户缓存范围参考 |
| [lib/clean/cache_guard.ps1](https://github.com/tw93/Mole/blob/3423ee037774fec5b20ec63b912a6df65a79675d/lib/clean/cache_guard.ps1) | 应用占用、配置目录和缓存边界保护思路 |
| [lib/core/file_ops.ps1](https://github.com/tw93/Mole/blob/3423ee037774fec5b20ec63b912a6df65a79675d/lib/core/file_ops.ps1)、[lib/core/base.ps1](https://github.com/tw93/Mole/blob/3423ee037774fec5b20ec63b912a6df65a79675d/lib/core/base.ps1) | 文件操作和基础安全检查参考 |

2026-10-29：CleanTool 将选定路径规则改写到 `src/native/rules.cpp`，未直接运行上游 PowerShell。改动包括双时间戳超过 7 天的限制、目录发现上限、文件身份复核和默认勾选范围。2026-11-08：整理公开源码、构建入口和归属说明。完整范围及未采用的规则见 [适配说明](work/mole-windows-rules.md)。

此归属不表示上游作者为 CleanTool 背书。Mole 的名称和品牌归其所有者。

## Microsoft Windows 接口与构建资料

以下是接口文档引用，不表示在项目中打包了 Windows 或 Visual Studio：

- [IEmptyVolumeCache](https://learn.microsoft.com/en-us/windows/win32/api/emptyvc/nn-emptyvc-iemptyvolumecache)：系统清理处理器接口，对应 `src/native/system-cleanup.cpp`。
- [WinSxS 与组件清理](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/clean-up-the-winsxs-folder)：保留的 DISM 组件维护入口参考，对应 `src/native/scanner.cpp`。
- [MSVC 命令行构建](https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line)：编译器与 Windows SDK 的安装说明。

## 包管理器维护命令

程序调用用户已安装的命令，不附带这些工具。命令及其缓存策略以各自文档为准：

- [pip cache](https://pip.pypa.io/en/stable/cli/pip_cache/)
- [npm cache](https://docs.npmjs.com/cli/v11/commands/npm-cache/)
- [uv 缓存管理](https://docs.astral.sh/uv/concepts/cache/)
