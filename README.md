# x64dbg-ai-plugin

为 [x64dbg](https://x64dbg.com) 开发的原生 C++ GUI 插件，把大语言模型（GitHub Copilot Chat / DeepSeek）直接嵌进调试器，
辅助逆向工作：反汇编 AI 解读、多会话持久化、RAG 长期记忆、启发式定位器、调用链追溯（正向 trace + 反向 callstack 采样）、跨版本会话浏览导入。

> 当前里程碑：**M3.6 跨 DB 会话浏览器** 已完成。详见 [`docs/features.md`](docs/features.md)、[`docs/development-log.md`](docs/development-log.md)。

---

## 核心特性

| 类别 | 能力 |
|---|---|
| **LLM 接入** | GitHub Copilot Chat（伪装 VSCode 客户端）+ DeepSeek 官方 API（OpenAI 兼容，含 reasoner）；UI 动态拉取模型列表，不硬编码 |
| **反汇编分析** | 反汇编窗口右键 → AI 分析当前地址；自动写入 RAG 向量库 |
| **多会话持久化** | 按目标 EXE SHA256 一个独立 sqlite 数据库；会话/消息/RAG 块全部本地保存；切换 Provider 模型按会话记录 |
| **RAG 长期记忆** | sqlite-vec 0.1.9 + GitHub Models `text-embedding-3-small` (1536-d)；每次提问自动 top-K 拼接相关历史片段 |
| **启发式定位器** | 6 类扫描：API 引用 / 字符串引用 / x64dbg 风格特征码 / 常量魔数 / 函数原型 / LLM 关键词扩展 |
| **调用链追溯 v2** | 4 种模式：Targeted Trace / Global Active / Global Passive / **CallStack 反向采样**；调用图折叠（系统模块兄弟聚合 + hits 降序） |
| **CallStack 采样** | 在叶子 API 下软断点 → 命中调 `GetCallStack` → FNV-1a hash 去重 → 达上限自动拆断点；无需先把链跑一遍 |
| **跨版本会话浏览** | 历史项目库浏览器（M3.6）；EXE 被打补丁 / 自动更新后 SHA 变，旧会话仍可一键导入到当前项目 |
| **聊天体验** | 流式渲染（16 ms 节流增量追加，长答复零闪烁）；Markdown + 代码块；流式期间禁用输入；仅贴底自动滚 |
| **凭据安全** | Copilot OAuth Token / DeepSeek API Key 走 Windows DPAPI 加密落盘 |
| **构建发布** | CMake + VS2022；双架构 `.dp32` / `.dp64`；vcpkg `*-windows-static-md` |

---

## 环境要求

| 组件 | 版本 |
|---|---|
| Visual Studio | 2022 (MSVC 19.4x，17.14+) |
| Qt | 5.12.12（msvc2017 32+64，`scripts\install_qt.ps1` 自动装） |
| vcpkg | 2025-12-16+，路径 `F:\vcpkg-master\vcpkg` |
| CMake | ≥ 3.21（项目锁 `F:\cmake`） |
| PowerShell | 7+ |
| x64dbg | snapshot 2026-04-20 或更新 |

依赖（vcpkg 自动管理）：`cpr` `nlohmann-json` `spdlog` `cmark` `openssl`；`sqlite-vec` 用 v0.1.9 amalgamation 内嵌（vcpkg 暂无）。

---

## 快速上手

```powershell
# 1. 一次性安装 Qt 5.12.12（约 5 分钟，双架构）
pwsh .\scripts\install_qt.ps1

# 2. 全量构建
pwsh .\scripts\build_all.ps1

# 3. 打包 .dp32/.dp64 到 release/
pwsh .\scripts\package_release.ps1 -Version 0.1.0
```

构建产物：

- `build-x64\bin\Release\x64dbg_ai_plugin.dp64`（~9.5 MB）
- `build-x86\bin\Release\x64dbg_ai_plugin.dp32`（~6.7 MB）

部署：拷到 x64dbg 的 `release\x64\plugins\` 与 `release\x32\plugins\`。

### 首次使用

1. 启动 x64dbg → `Plugins` 菜单出现 `x64dbg AI`
2. 打开 AI 助手面板 → 顶部选 Provider（Copilot / DeepSeek）→ 点 `登录` 或 `设置 Key`
3. 附加任意目标程序 → 反汇编窗口右键 `AI 分析当前地址`

---

## 仓库结构

```
src/
  plugin/    x64dbg 插件入口、回调、菜单
  ai/        chat provider 抽象 / Copilot / DeepSeek / OAuth / embedding / SSE 解析
  debugger/  反汇编上下文采集
  locator/   启发式定位器（6 类 scanner）
  trace/     trace_recorder + callstack_tracer + call_graph
  storage/   session_store + project_context + project_browser（跨 DB 只读）
  ui/        assistant_panel + chat_view + 各对话框（locator/trace/history/login/api_key）
  util/      logging + paths + config + secret_store + hashing
cmake/       FindX64DbgSDK / Qt5Setup / PluginPackaging
resources/   主题 QSS + lucide-icons SVG
scripts/     install_qt / build_all / package_release
docs/        架构 / 功能 / 开发日志 / 已知问题
tests/       trace_demo（NOINLINE 标定的调用链测试程序）
third_party/ pluginsdk（x64dbg 头文件） + sqlite-vec amalgamation
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 模块依赖、数据流、线程模型、关键设计决策 |
| [`docs/features.md`](docs/features.md) | 功能逐项详细说明（含使用方法） |
| [`docs/development-log.md`](docs/development-log.md) | 各里程碑遇到的问题与解决方法 |
| [`docs/known-issues.md`](docs/known-issues.md) | 未解决问题、技术债、平台限制 |

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
```

---

## 许可

待定。
