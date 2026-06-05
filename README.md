# x64dbg-ai-plugin

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows-blue.svg)](#)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](#)
[![Qt 5.12.12](https://img.shields.io/badge/Qt-5.12.12-green.svg)](#)

为 [x64dbg](https://x64dbg.com) 开发的原生 C++ / Qt 插件，把大语言模型（GitHub Copilot Chat / DeepSeek）直接嵌进调试器，
辅助逆向工作：反汇编 AI 解读、Agent 自主调试（**81 个工具**，K-43 已加运行态守卫）、多会话持久化、RAG 长期记忆、启发式定位器、调用链追溯（正向 trace + 反向 callstack 采样）、跨版本会话浏览导入。

> 状态：M3.6 跨 DB 会话浏览器 + K-43 Agent 工具运行态守卫已完成。详见 [`docs/features.md`](docs/features.md)、[`docs/development-log.md`](docs/development-log.md)。

---

## 核心特性

| 类别 | 能力 |
|---|---|
| **LLM 接入** | GitHub Copilot Chat（OAuth 设备码登录）+ DeepSeek 官方 API（OpenAI 兼容，含 reasoner）；UI 动态拉取模型列表，不硬编码 |
| **Agent 自主调试** | 81 个工具：寄存器/内存读写、断点管理、反汇编、调用栈、模块/导入表/导出表、表达式求值、脚本执行、debug 会话控制等；运行态自检（K-42/K-43）防止 LLM 拿 stale 数据乱动 |
| **反汇编分析** | 反汇编窗口右键 → AI 分析当前地址；自动写入 RAG 向量库 |
| **多会话持久化** | 按目标 EXE SHA256 一个独立 sqlite 数据库；会话/消息/RAG 块/工具调用记录全部本地保存 |
| **RAG 长期记忆** | sqlite-vec 0.1.9 + GitHub Models `text-embedding-3-small` (1536-d)；每次提问自动 top-K 拼接相关历史片段 |
| **启发式定位器** | 6 类扫描：API 引用 / 字符串引用 / x64dbg 风格特征码 / 常量魔数 / 函数原型 / LLM 关键词扩展 |
| **调用链追溯** | 4 种模式：Targeted Trace / Global Active / Global Passive / CallStack 反向采样；调用图折叠（系统模块兄弟聚合 + hits 降序） |
| **跨版本会话浏览** | 历史项目库浏览器（M3.6）；EXE 被打补丁 / 自动更新后 SHA 变，旧会话仍可一键导入到当前项目 |
| **聊天体验** | 流式渲染（16 ms 节流增量追加）；Markdown + 代码块；流式期间禁用输入；仅贴底自动滚 |
| **凭据安全** | Copilot OAuth Token / DeepSeek API Key / Embedding PAT 走 Windows DPAPI 加密落盘 |
| **构建发布** | CMake + VS2022；双架构 `.dp32` / `.dp64`；vcpkg `*-windows-static-md` |

---

## 环境要求

| 组件 | 版本 |
|---|---|
| Windows | 10 / 11 (x64) |
| Visual Studio | 2022 (MSVC 19.4x，17.14+) |
| Qt | 5.12.12（msvc2017 32+64，`scripts\install_qt.ps1` 可自动装） |
| vcpkg | 2025-12-16+ |
| CMake | ≥ 3.21 |
| PowerShell | 7+ |
| x64dbg | snapshot 2026-04-20 或更新（需要其 `pluginsdk/`） |

依赖（vcpkg 自动管理）：`cpr` `nlohmann-json` `spdlog` `cmark` `openssl`；`sqlite-vec` 用 v0.1.9 amalgamation 内嵌（vcpkg 暂无）。

---

## 快速上手

### 1. 准备依赖

- 安装 Visual Studio 2022（含 "Desktop development with C++" workload）
- 安装 [vcpkg](https://github.com/microsoft/vcpkg) 并设置环境变量 `VCPKG_ROOT` 指向 vcpkg 根目录
- 准备 x64dbg snapshot（[官网](https://x64dbg.com/) 下载），把 snapshot 里的 `pluginsdk/` 拷到本仓 `third_party/pluginsdk/`

### 2. 一键安装 Qt 5.12.12（可选）

```powershell
pwsh .\scripts\install_qt.ps1
```

或自行准备 Qt 5.12.12 msvc2017_64 + msvc2017，并设置环境变量 `QT5_ROOT` 指向 Qt 安装目录。

### 3. 全量构建

```powershell
pwsh .\scripts\build_all.ps1
```

### 4. 打包 + 部署

```powershell
pwsh .\scripts\package_release.ps1 -Version 0.1.0
```

构建产物：

- `build-x64\bin\Release\x64dbg_ai_plugin.dp64`（~9.8 MB）
- `build-x86\bin\Release\x64dbg_ai_plugin.dp32`（~6.9 MB）

把 `.dp32/.dp64` 拷到 x64dbg 安装目录的 `release\x32\plugins\` 与 `release\x64\plugins\` 即可。
也可以让 CMake 直接 install 到 x64dbg 目录：

```powershell
cmake -B build-x64 -A x64 -DX64DBG_RELEASE_DIR="D:/x64dbg/release" -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-x64 --config Release --target install
```

### 5. 首次使用

1. 启动 x64dbg → `Plugins` 菜单出现 **x64dbg AI**
2. 打开 AI 助手面板 → 顶部选 Provider（Copilot / DeepSeek）→ 点 **登录** 或 **设置 Key**
3. 附加任意目标程序 → 反汇编窗口右键 → **AI 分析当前地址**
4. 想体验 Agent：直接在聊天框说"分析下当前函数干啥的"或"找下程序里的字符串解密逻辑"

---

## 仓库结构

```
src/
  plugin/    x64dbg 插件入口、回调、菜单
  ai/        chat provider 抽象 / Copilot / DeepSeek / OAuth / embedding / SSE
  ai/tools/  81 个 Agent 工具实现（按类别分文件）
  debugger/  反汇编上下文采集
  locator/   启发式定位器（6 类 scanner）
  trace/     trace_recorder + callstack_tracer + call_graph
  storage/   session_store + project_context + project_browser（跨 DB 只读）
  ui/        assistant_panel + chat_view + 各对话框
  util/      logging + paths + config + secret_store + hashing + encoding
cmake/       FindX64DbgSDK / Qt5Setup / PluginPackaging
resources/   Dark Modern 主题 QSS + lucide-icons SVG
scripts/     install_qt / build_all / package_release
docs/        架构 / 功能 / 开发日志 / 已知问题 / Agent 能力评估
tests/       trace_demo / agent_demo / reasoning_demo（NOINLINE 标定的测试靶子）
samples/     login_demo（plain/xor/antidbg 三档登录靶子，供新手练手）
third_party/ pluginsdk（x64dbg 头文件，需自行从 snapshot 拷入）+ sqlite-vec amalgamation
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 模块依赖、数据流、线程模型、关键设计决策 |
| [`docs/features.md`](docs/features.md) | 功能逐项详细说明（含使用方法） |
| [`docs/development-log.md`](docs/development-log.md) | 各里程碑遇到的问题与解决方法 |
| [`docs/known-issues.md`](docs/known-issues.md) | 未解决问题、技术债、平台限制 |
| [`docs/agent-capability-assessment.md`](docs/agent-capability-assessment.md) | Agent 81 工具能力评估 |

---

## 数据存放位置

```
%APPDATA%\x64dbg-ai-plugin\
  config.json                  应用配置（只读，写回未实现）
  provider.txt                 当前激活的 Provider
  logs\plugin.log              spdlog 文件输出
  projects\<sha256>.db         按目标 EXE 哈希隔离的会话+RAG 数据库
  secrets\
    copilot_oauth_token.bin    DPAPI 加密
    deepseek_api_key.bin       DPAPI 加密
    github_pat.bin             DPAPI 加密（Embedding 用）
```

---

## 致谢

本项目站在以下开源/商业项目的肩膀上：

- **[x64dbg](https://x64dbg.com)** — 强大的 Windows 调试器及其插件 SDK
- **[Qt 5.12](https://www.qt.io)** — UI 框架
- **[sqlite-vec](https://github.com/asg017/sqlite-vec)** — sqlite 向量扩展（RAG 检索）
- **[cpr](https://github.com/libcpr/cpr)** — C++ HTTP 客户端
- **[nlohmann/json](https://github.com/nlohmann/json)** — JSON 解析
- **[spdlog](https://github.com/gabime/spdlog)** — 日志库
- **[cmark](https://github.com/commonmark/cmark)** — CommonMark 渲染
- **[OpenSSL](https://www.openssl.org)** — TLS / 哈希
- **[Lucide Icons](https://lucide.dev)** — UI 图标

LLM 服务：[GitHub Copilot](https://github.com/features/copilot) / [DeepSeek](https://www.deepseek.com) / [GitHub Models](https://github.com/marketplace/models)（Embedding）。

---

## 贡献

欢迎 Issue 与 Pull Request。提 PR 前请：

1. 双架构都能编译（`scripts\build_all.ps1` 通过）
2. commit message 用英文 subject + 英文 body（中文 body 在 Windows + git-for-windows + GBK 编码下会糊掉）
3. 不提交个人路径、私密配置、`%APPDATA%\x64dbg-ai-plugin` 里的运行时数据

---

## 许可

[MIT License](LICENSE) © 2026 houxianzhi
