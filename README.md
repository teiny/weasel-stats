# 小狼毫输入统计补丁

这是一个基于 [小狼毫（Weasel）](https://github.com/rime/weasel) 的 Windows 输入统计功能分支。项目保留小狼毫原有的输入法、部署和用户数据同步能力，在其基础上增加严格上屏统计、多设备数据合并和本地可视化报表。

本项目不是独立输入法，也不是 Rime 输入方案或外部键盘记录工具。它直接扩展小狼毫，只接收 Rime 已确认并成功上屏的提交文本，不监听其他输入法或普通键盘输入。

## 与小狼毫的关系

- 上游项目是 `rime/weasel`，本仓库是面向输入统计功能的派生版本。
- 小狼毫仍负责 TSF 输入、候选选择、上屏、托盘菜单、部署和同步流程。
- `librime` 仍作为小狼毫的子模块使用；当前统计功能不需要修改 `librime`。
- `WeaselServer` 在文本成功提交后，把统计事件发送给独立的 `WeaselStats` 进程。
- `WeaselStats` 负责计数、SQLite 持久化、同步合并和报表窗口。
- 托盘菜单和 TSF 菜单中的“今日输入 N 字”是统计报表入口。

```text
Rime / WeaselTSF
        │ 成功上屏
        ▼
  WeaselServer ──受限 IPC──> WeaselStats ──> 本地 SQLite
        │                         │
        │ 菜单入口                ├──> 同步目录 SQLite 集合合并
        └─────────────────────────└──> WebView2 离线统计报表
```

统计属于附加能力。`WeaselStats` 启动、通信、写库、同步或报表异常时，必须结束当前统计操作并保持小狼毫主输入流程正常，不进行无限重试。

## 功能特性

### 严格上屏统计

- 只有 Rime 最终成功提交到应用程序的文本才会计入统计。
- 候选栏中尚未上屏的候选词、组合过程和普通按键不会计入统计。
- 一个汉字计为 1 字，一个英文单词也计为 1 字。
- 标点、空白等内容不计入总字数。
- 今日概览统一显示为“今日输入 N 字”，不单独拆分中英文。
- 数据层保留汉字数和英文单词数，供后续详情统计使用。

### 按日存储

- 默认数据库位于小狼毫用户数据目录：`weasel-input-statistics.sqlite3`。
- 数据按日期和设备保存，适合长期累计。
- 当某一天没有开机或没有使用输入法时，报表中按 0 字显示，不跳过日期。
- 数据库同时保留每日词条聚合，为后续词频详情提供数据基础。

### 多设备同步

- 本地用户数据目录保存一份 SQLite 数据库。
- Rime 同步目录保存一份供所有设备共享的统计数据库。
- 小狼毫完成用户数据维护后，由 `WeaselServer` 触发 `WeaselStats` 执行统计同步。
- 同步采用集合合并：本机数据合并到共享数据库，共享数据库中的其他设备数据再合并回本机。
- 合并不依赖单一的“最后同步时间戳”覆盖，因此同步盘返回旧文件时不会直接抹掉较新的本地记录。
- 多设备数据按设备标识保留，可在报表中查看全部设备或筛选指定设备。

### 本地统计报表

点击托盘菜单或 TSF 菜单中的“今日输入 N 字”可打开报表。日折线是默认视图。

报表分为两个标签页：

- **折线视图**：展示周期内每天、每月或每年的输入趋势。
- **日历视图**：以日、月或年为格子展示周期字数，使用热力颜色标记文本，并高亮当前日、月或年。

两个视图都支持以下统计维度：

- **日**：展示一个自然月内每天的数据，可切换上一个或下一个自然月。
- **月**：展示一个自然年内每个自然月的数据，可切换上一个或下一个自然年；未来月份不显示。
- **年**：展示有统计记录以来的自然年，每页最多 10 年，可前后翻页。
- **设备**：查看全部设备，或筛选一个及多个设备。

项目不提供周维度。报表窗口采用固定尺寸，不显示页面滚动条；日历格中的数值右对齐且不重复显示“字”单位。

### 脱机可用

- 图表使用 ECharts，并作为资源嵌入 `WeaselStats.exe`，运行时不访问 CDN。
- HTML、CSS 和 JavaScript 同样嵌入程序资源。
- 报表需要系统安装 Microsoft Edge WebView2 Runtime。
- 数据读取、聚合和渲染均在本机完成，不上传输入统计数据。

### 故障隔离

- `WeaselStats` 与 `WeaselServer` 使用独立进程，统计程序崩溃不会成为输入法主服务的一部分。
- 统计 IPC、数据库读取和菜单概览均采用失败即降级的处理方式。
- 报表不可用时只显示提示，不影响继续输入。
- 同步失败不会无限重试；当前操作结束后向用户提示失败。

## 运行要求

- Windows 10 或 Windows 11。
- 已安装与补丁版本一致的小狼毫。
- 系统提供 `winsqlite3.dll`。
- 已安装 Microsoft Edge WebView2 Runtime，用于显示统计报表。

补丁必须与目标小狼毫的 PE 固定文件版本一致。部署脚本会在停止服务和替换文件之前检查版本；任一补丁版本缺失或不一致时直接终止。

## 构建依赖

编译前需要安装 Visual Studio 2022 或 Build Tools 2022，并在 Visual Studio Installer 中选择“使用 C++ 的桌面开发”工作负载。安装详细信息至少应包含：

- MSVC v143 - VS 2022 C++ x64/x86 生成工具
- Windows 10 SDK 或 Windows 11 SDK

具体步骤参见 [Microsoft 官方 C++ 安装说明](https://learn.microsoft.com/zh-cn/cpp/build/vscpp-step-0-installation?view=msvc-170)。一键编译脚本检测不到这些组件时，也会在窗口中显示相同的安装提示。

统计报表新增以下本地构建依赖：

- Boost 1.84.0
- librime x86/x64 开发文件
- Microsoft WebView2 SDK
- Apache ECharts

Boost、WebView2 SDK 和 ECharts 可通过以下脚本下载；固定版本文件会校验 SHA-256：

```powershell
powershell -ExecutionPolicy Bypass -File .\WeaselStats\fetch_dependencies.ps1
```

ECharts 只在构建阶段下载，最终运行时使用嵌入的离线资源。

### 一键编译和打包

在源码根目录双击 `build_stats_patch.cmd`。脚本不需要管理员权限，并会按顺序执行：

1. 自动查找 Visual Studio 2022 MSBuild。
2. 下载并校验缺失的 Boost 1.84.0、WebView2 SDK 和 ECharts 构建依赖。
3. 编译 Boost Release x86/x64 静态库；如需使用已有 Boost，可分别通过 `WEASEL_PATCH_BOOST_X64`、`WEASEL_PATCH_BOOST_X86` 环境变量指定源码目录。
4. 获取缺失的 librime x86/x64 开发文件，且不会停止正在运行的已安装小狼毫服务。
5. 根据 `build.bat` 的版本号生成 `weasel.props`。
6. 编译 Release x64 `WeaselStats.exe`。
7. 编译 Release x64 `weaselx64.dll` 和 Release Win32 `weasel.dll`。
8. 校验三个构建物的 PE 固定文件版本完全一致，并将它们和一键部署脚本打包为：

```text
output\weasel-input-statistics-patch-<版本>.zip
```

ZIP 根目录只包含 `WeaselStats.exe`、`weasel.dll`、`weaselx64.dll` 和 `deploy_stats_patch.cmd`。编译或校验失败时不会生成或覆盖最终 ZIP。

## 增量补丁部署

源码根目录提供 `deploy_stats_patch.cmd`。一键编译脚本会将它与三个构建物一起放入增量补丁 ZIP，解压后的目录包含：

- `WeaselStats.exe`
- `weasel.dll`
- `weaselx64.dll`
- `deploy_stats_patch.cmd`

将一键部署脚本与三个补丁文件放在同一目录，双击 `deploy_stats_patch.cmd`。脚本会申请管理员权限，定位小狼毫安装目录，执行版本检查、原文件备份、替换、二进制校验、失败回滚和服务重启。

已经运行的应用程序可能仍持有旧版 TSF DLL。部署成功后应重启相关应用；需要让所有进程统一加载新版本时，请注销 Windows 后重新登录。

## 尚未提供的详情功能

以下内容属于后续方向，不应视为当前已经完成的功能：

- 中英文数量的独立详情页面。
- 当天或指定周期输入最多的词条排行。
- 基于候选栏删除键次数的错误字母统计。
- 更多导出或自定义图表能力。

## 上游项目与许可证

- 小狼毫上游：[rime/weasel](https://github.com/rime/weasel)
- Rime 项目主页：[rime.im](https://rime.im)
- Rime 配置指南：[Customization Guide](https://github.com/rime/home/wiki/CustomizationGuide)

本项目沿用小狼毫的 GPLv3 许可证，详情见 [LICENSE.txt](LICENSE.txt)。小狼毫及其依赖的版权、许可证和贡献者信息归各自项目所有。
