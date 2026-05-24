# 功能说明

按里程碑 + 模块组织。所有功能均已在 `build-x64` / `build-x86` 双架构通过构建并部署。

---

## 1. LLM Provider 接入

### 1.1 GitHub Copilot Chat（M2.1 / M3.0）

- **鉴权方式**：GitHub Device Flow（用户在浏览器输入 8 位 code 完成授权）
- **客户端身份**：伪装 VSCode
  - `client_id = 01ab8ac9400c4e429b23`
  - Header: `Copilot-Integration-Id: vscode-chat`
- **凭据存储**：`%APPDATA%\x64dbg-ai-plugin\secrets\copilot_oauth_token.bin`（DPAPI 加密）
- **Token 刷新**：每次请求前用 long-lived OAuth token 换 short-lived chat token，自动缓存到过期
- **TLS 校验**：libcurl 启用 `CURLSSLOPT_NO_REVOKE`（部分企业网无法访问 CRL 服务器）

### 1.2 DeepSeek（M3.3）

- **鉴权方式**：用户在「设置 Key」对话框直接粘贴 API Key
- **存储**：`secrets\deepseek_api_key.bin`（DPAPI）
- **模型支持**：`deepseek-chat`、`deepseek-reasoner`（reasoner 的 `reasoning_content` 与 `content` 均通过 `onDelta` 回调流出）
- **接口**：OpenAI 兼容，`https://api.deepseek.com/v1/chat/completions`

### 1.3 Provider 切换

- 顶部下拉切换；持久化到 `%APPDATA%\x64dbg-ai-plugin\provider.txt`
- 切换后自动清空模型下拉、重新拉取当前 provider 的 `/models` 列表
- 抽象接口 `IChatProvider`（`src/ai/chat_provider.h`）：`stream / complete / listModels / defaultModel`

### 1.4 Embedding

- 独立走 **GitHub Models API**（`https://models.github.ai/inference`）
- 模型：`text-embedding-3-small`，维度 1536
- 需要用户提供 GitHub PAT（与 Copilot OAuth Token 不同）
- 失败时自动降级：跳过 RAG 直接走原始 prompt

---

## 2. 反汇编 AI 分析（M2.4 / M3.0 / M4.6f）

### 入口
- **反汇编窗口右键 → `AI ▶`** 弹出子菜单，列出所有 `showInContextMenu=true` 的 Agent 预设（M4.6f）
  - 子菜单条目动态由 `PresetStore` 重建；预设增删/勾选变更后自动重新挂载（`rebuildDisasmAiSubmenu()`）
  - 已删除老的 `AI 分析当前地址`（被预设系统覆盖）
  - 保留独立条目：`AI 追溯此函数调用链`（调用 TraceDialog）
- **Plugins 菜单 → x64dbg AI**：相同 AI ▶ 子菜单
- **AssistantPanel 顶部「Agent」下拉**：手动选预设；下方输入框回车 = 跑当前激活预设

### 行为
1. 拿到当前 CIP / 模块 / 选区，按预设 `userTemplate` 展开 `{{cip}}` `{{module}}` `{{selection}}` `{{disasm}}` `{{user}}` 占位符
2. 走 **Agent loop**（M4）：LLM 可自主调工具（read_memory / disasm_at / list_xrefs_to / ...）多步推理后再回答
3. 流式结果实时渲染：`content` → 助手气泡；`reasoning_content`（thinking 模型）→ ReasoningBlock 折叠面板；`tool_calls` → ToolCallCard 卡片
4. 完成后整段对话（含 tool 消息）持久化到当前会话

### 退化路径
- 预设关掉所有工具或 provider 不支持 function calling（Copilot 自动降级）→ 单轮 streamChat，行为等同 M3 老版本
- 老 `AI 分析当前地址` 的固定 system prompt 已迁移到出厂预设「分析当前指令」

---

## 3. 多会话管理（M2.2 / M2.3）

### 数据库
- 每个目标 EXE 一个 `<sha256>.db` 文件
- 启动调试时 `ProjectContext::onDebugStart` 计算主模块 SHA256 → 打开对应 db（不存在则建）
- 停止调试时 `onDebugStop` 关闭 store

### UI（左侧 SessionListWidget）
- 列出当前 db 全部会话（按 `updated_at DESC`）
- 顶部「+ 新建」按钮
- 右键菜单：重命名 / 删除
- 双击切换；切换会话时清空对话区、按 id ASC 重放 `messages`
- 每个会话单独记录当时使用的模型；切回时自动恢复 modelBox 选中项

---

## 4. RAG 长期记忆（M2.4）

### 写入
- 反汇编分析时自动 `addChunk("asm", va, mdText, embedding)`
- 未来扩展点：字符串扫描结果、用户笔记、定位器命中也可写入

### 检索
- 每次提问前：`embed(userPrompt) → searchSimilar(qEmb, topK=4)` 用 sqlite-vec KNN
- 拼接格式：
  ```
  # Project context (top-N related snippets retrieved by semantic search)

  ## [1] kind=asm va=0x... distance=0.1234
  <chunk text>

  ## [2] ...
  ```

### 失败容忍
- PAT 未配置 / embedding 接口 502：跳过 RAG，原 prompt 直送

---

## 5. 启发式定位器（M3.1）

`AssistantPanel` 顶部 `🔍 扫描` 按钮 → `LocatorDialog`。

| Scanner | 工作方式 |
|---|---|
| **API 引用** | 用户输入函数名（如 `send`）→ `DbgGetExportFromName` → 全部模块 X-Ref 收集 callers |
| **字符串引用** | 用户输入字符串 → x64dbg `strref` 命令；支持子串 |
| **特征码** | x64dbg 风格 `?` 通配符；调 `findall` |
| **常量/魔数** | 直接搜索 4/8 字节立即数 |
| **函数原型** | 按 PE 导出表过滤参数个数（启发式） |
| **LLM 关键词扩展** | 用户输入关键词描述 → LLM 输出 API/字符串候选 → 自动跑前面几类 |

结果展示在表格里：模块名 / VA / 命中类型 / 上下文摘要。双击跳转到反汇编窗口。

### 5.1 用户关键字输入（M3.1.k）

对话框新增一行 `关键字:` QLineEdit（QSettings 持久化，键 `x64dbg-ai-plugin/locator/userKeywords`）。
分隔符：`,` / `;` / 换行；**空格保留**（用于 hex pattern）。

| Scanner | 关键字语义 |
|---|---|
| **String** | 大小写不敏感 substring；命中则 +40 分，evidence 追加 `[kw:xxx]`；空关键字 → 沿用内置词表 |
| **API** | 内置敏感表未命中时，对归一化 IAT 名做 substring 匹配；命中则收录，`category="user-keyword"`，score=70 |
| **Pattern** | 自动判别：形如 `DE AD BE EF` / `48 8B ?? E8` 视为 hex pattern（`??`/`?` = 整字节通配）；否则按 ASCII + UTF-16LE 双形态扫描所有节，命中 `category="user-keyword"`，score=75 |

实现：`src/locator/{string,api,pattern}_scanner.{h,cpp}` 各自 `scan(...)` 加 `userKeywords` 参数；
`LocatorEngine::Options::userKeywords` 统一向下传递（API/String 在 engine 内小写化后下传，Pattern 保留原始用于 hex 解析）。

---

## 6. 调用链追溯 v2（M3.2 / M3.5）

`AssistantPanel` 顶部 `🌿 追溯` 按钮 → `TraceDialog`。**4 种工作模式**，QStackedWidget 切换 UI。

### 6.1 Targeted Trace（瞄准函数）
- 用户输入函数地址；自动 `bp <addr>` 等命中
- 命中后开 `TraceIntoConditional 0` 单步追到函数返回（SP 严格抬高）
- 期间每条指令分类为 CALL / RET / 普通，构建 `CallNode` 树

### 6.2 Global Active Trace
- 在用户当前 EIP/RIP 处开 `TraceIntoConditional 0`
- 没有终止条件，按"采集 N 步后停"或用户手动 Stop

### 6.3 Global Passive
- 不主动 trace，只挂 `cbTraceExecute` 监听 x64dbg 用户手动 Trace 时的事件
- 适合"我自己单步操作时顺便录下来"

### 6.4 CallStack 反向采样（M3.5，**重点**）
- 适用：trace 跟不下去的系统叶子 API（`send`、`recv`、`CreateFileW`…）
- 流程：
  1. 用户输入叶子 API 地址 + 最大采样次数（默认 32）
  2. `bp <addr>` 等命中
  3. 命中后调 `DbgFunctions()->GetCallStack(&cs)`（RtlVirtualUnwind 同步采）
  4. 序列化 frames（comment 优先，fallback `DbgGetLabelAt`，再 fallback `mod+0xRVA`）
  5. FNV-1a 64 hash 按 (addr, from, to) 三元组去重；hits 累加
  6. 命中数达上限 → `bc <addr>` 自动拆 → 写入采样表
  7. 每次命中都显式 `DbgCmdExec("run")` 恢复
- UI：trie 树展示去重后的 unique stacks；hits 降序；缩进显示路径深度

### 调用图折叠
- 兄弟节点：相同 (from→to) 自动合并，hits 累加
- 连续系统模块兄弟：折叠为一个灰色节点（如 `ucrtbase × 7`）
- 系统模块清单见 `src/trace/call_graph.cpp` 的 `kSystemMods`：含 ucrtbase / msvcrt* / vcruntime* / ntdll / kernelbase / win32u / ws2_32 / kernel32 / ...（约 30 个）

### AI 分析
- 「AI 分析此调用图」按钮：把折叠后的调用图序列化成 markdown，加专用 system prompt 发到 LLM
- CallStack 模式有独立 prompt 措辞（强调"这是反向 callstack 采样而非正向 trace"）

---

## 7. 聊天体验（ChatView，M3 + 闪烁修复）

### 流式渲染协议（**关键性能修复**）
- 起点：`appendAssistantHeader` 一次 `setHtml` 出空气泡 + `streamCursor_` 指文档末
- 每个 delta：入 `pendingDelta_` + `flushTimer_->start(16)`（16 ms 单次触发节流）
- 触发：`flushPendingDelta` → `streamCursor_.insertText(local)`（**仅增量追加，不重排历史**）
- `finalize`：停 timer + flush 残余 + 清 cursor + 一次性 rerender 补"助手"footer
- 任何 `setHtml` 前必须清 `streamCursor_`（QTextCursor 在 setHtml 后立即失效）

### 滚动策略
- `userAtBottom_`（容差 4 px）+ `programmaticScroll_` 屏蔽
- **仅当用户原本贴底**才自动滚到末尾；否则不打扰用户阅读历史

### 输入控制
- 流式期间 `setInputEnabled(false)`（输入框 + 发送按钮一起禁用）
- `finalize` 时恢复

### 渲染样式
- 用户消息：右侧蓝色气泡 + user icon
- 助手消息：左侧灰色气泡 + bot icon
- 系统提示：居中灰色斜体
- 代码块：等宽字体 + 深色背景 + 1px 边框
- 链接：可点；外链由系统浏览器打开

---

## 8. 跨版本会话浏览器（M3.6 ★最新）

`AssistantPanel` 顶部 `📁 历史` 按钮 → `HistoryDialog`。

### 解决的问题
游戏客户端 / 频繁打补丁的程序，每次更新 EXE 的 SHA256 都变 → 旧会话被孤立在历史 db 文件里加载不到。

### UI 布局
| 区域 | 内容 |
|---|---|
| 左 | 历史 db 列表：`● SHA前12 · 288 KB · 2026-05-13 19:56`；当前活动项目加粗 ●，按 mtime 倒序 |
| 中 | 选中 db 的 sessions 表：ID / 标题 / 模型 / 最后更新 |
| 右 | 选中 session 的全部 messages 预览（`<pre>` + HTML 转义，不渲染 markdown 避免触发外链） |
| 底 | `导入到当前项目` + `关闭` |

### 导入语义
- **只拷贝 messages**（不带 chunks/embeddings），避免污染当前 RAG
- 在当前活动 db 里 `createSession("原标题（导入自 sha前12）", 原model)` → 遍历 `appendMessage`
- 导入完成弹提示并 `emit imported()`，AssistantPanel 收到信号刷新左侧会话列表
- 对话框**不关闭**，方便连续浏览/导入多个

### 防呆
- 选中的 db == 当前活动 db：按钮禁用，提示「已是当前项目」
- 当前未在调试：按钮禁用，提示「无目标项目可导入」
- 老 db 没有 sessions：按钮禁用

### 实现要点
- 完全独立的 `ProjectBrowser`（`src/storage/project_browser.cpp`），不复用 `SessionStore`
- 用 sqlite3 URI `file:<path>?mode=ro&immutable=1` 只读打开任意 .db
- 不触发 sqlite-vec 加载、不写 schema、对老库零侵入

---

## 9. 配置与持久化

| 路径 | 用途 | 写入方 |
|---|---|---|
| `%APPDATA%\x64dbg-ai-plugin\config.json` | 应用配置（HTTP 超时、log 级别等） | 只读（写回未实现） |
| `%APPDATA%\x64dbg-ai-plugin\provider.txt` | 当前激活的 Provider 名 | `ProviderManager::setProvider` |
| `%APPDATA%\x64dbg-ai-plugin\logs\plugin.log` | spdlog 文件输出 | `XAI_LOG_*` 宏 |
| `%APPDATA%\x64dbg-ai-plugin\logs\write_audit.log` | S3 写工具审计（JSON 一行一条；rotating 4 MB×10） | `util/logging.cpp::auditLog` |
| `%APPDATA%\x64dbg-ai-plugin\projects\<sha>.db` | 会话 + RAG 数据库 | `SessionStore` |
| `%APPDATA%\x64dbg-ai-plugin\agent_presets.json` | Agent 预设（schemaVersion=8，S4 后 analyze-function 含全部 9 个写工具） | `PresetStore` |
| `%APPDATA%\x64dbg-ai-plugin\secrets\*.bin` | DPAPI 加密的 token/key | `SecretStore` |

### HTTP 超时（M3.3 修复后）
- 普通请求：`timeoutMs = 30000`
- 流式请求：`streamTimeoutMs = 600000`（10 分钟）+ `streamLowSpeedSec = 30`（30 秒无数据视为断流）

---

## 10. 测试程序

`tests/trace_demo/`：一个 NOINLINE + `/utf-8` 编译的简单业务流水线，用来回归 trace 各模式。

- 主入口 `ProcessOrder`：依次调 `ValidateInput / Compute / FormatOutput / NotifyUser`
- x64 Release 实测：Targeted Trace 43306 步成功 RETURNED ✅
- 构建：`tests/trace_demo/build/Release/trace_demo.exe`

### 真实程序回归
`JX3ClientX64.exe` 上对 `ws2_32.send` 做反向 callstack 采样：32 hits / 2 unique stacks；最深业务 caller 是 `jx3clientx64.ReportClientProfileInfo+618CEB`，已确认为 send 薄封装。

### agent_demo（M4 Agent 工具链回归）
`tests/agent_demo/`：4 道"必须用工具才能解出"的小题，对应 read_memory / disassemble / list_xrefs_to / get_string 全工具链覆盖。

- S1 XOR 解密：key `3F A1 5C 7E B2`，公式 `cipher[i] = plain[i] ^ key[i%5] ^ (i & 0xFF)`
- S2 CRC32 校验：poly `0xEDB88320`，license `"HELLO-WORLD-2026"`
- S3 函数指纹：通过 disasm 序列识别已知库函数
- S4 魔数定位：`DE AD BE EF CA FE BA BE` / `4D 5A 90 00` / `13 37 C0 DE`
- 控制台启动时 `SetConsoleOutputCP(CP_UTF8)`，避免 cmd 默认 GBK 把 UTF-8 字符串字面量显示成 mojibake（与 reasoning_demo 同根，详见 development-log D-02）
- 构建：`tests/agent_demo/build/Release/agent_demo.exe`（双架构）

### reasoning_demo（M4.6 Reasoning UI + thinking 模型回传回归）
`tests/reasoning_demo/`：3 道"命名中性、必须长篇推理"的小函数，专门驱动 ReasoningBlock 折叠面板与 `reasoning_content` 协议契约。

- T1 `Mystery1(13)` → 期望 16，本质 next-power-of-2 ceiling
- T2 `Mystery2(10)` → 类 Fibonacci + `(n & 1)` 偏移，需展开 4-5 项才能确认
- T3 `Mystery3(840, 360)` → 期望 120，GCD 辗转相除
- 回归点：thinking 模型的 `reasoning_content` 完整通道（SSE 拆字段 → UI 折叠 → 序列化回传）。M-1 (2026-05-24) probe_reasoning 实测：回传 / 剥离 deepseek-reasoner 都接受（HTTP 200），插件保留回传以兼容未来协议收紧
- 控制台启动时 `SetConsoleOutputCP(CP_UTF8)`，避免 cmd 默认 GBK 把 UTF-8 字符串字面量显示成 mojibake
- 构建：`tests/reasoning_demo/build/Release/reasoning_demo.exe`（双架构）

---

## 11. Agent 多轮工具调用（M4 ★最新）

LLM 主导的多步推理。给 LLM 一组工具，让它自己决定"先看什么、再算什么、何时回答"。

### 工具清单（S4 后 24 个：14 个只读 + 1 个控制 + 9 个写）

| 类别 | 工具 | 说明 |
|---|---|---|
| 基础读取 | `read_memory(addr, size)` | 单次返回硬截断 64 KB；超限自动 truncated 标记。S0-H1 后 `size` 接受 number / "256" / "0x100" |
|  | `read_string(addr, max_len, encoding)` | ASCII/UTF-8/UTF-16 |
|  | `get_registers()` | 当前线程通用寄存器快照 |
|  | `get_module_info(name_or_addr)` | base / size / entry / pdb 路径 |
|  | `list_modules()` | 进程内全部已加载模块 |
| 静态分析 | `disasm_at(addr, count)` | 反汇编 N 条；S0-H1 后 `lines` 接受 number/字符串 |
|  | `list_functions(module)` | 已识别函数列表 |
|  | `list_xrefs_to(addr)` / `list_xrefs_from(addr)` | x64dbg 内部 X-Ref 表 |
|  | `find_pattern(pattern, module?)` | x64dbg 风格 `?` 通配 |
| 动态上下文 | `get_call_stack()` | 当前线程 unwound frames（RtlVirtualUnwind） |
|  | `get_thread_list()` | 当前进程线程列表 |
| **S1 新增** | `eval_expression(expr)` | 把 `[rbp+8]+10`、`kernel32.GetProcAddress` 等交给 `DbgEval` 求值，返回 hex+dec |
|  | `list_breakpoints(type?)` | 列出 software/hardware/memory/dll/exception 断点，每条含 addr/enabled/active/hitCount/mod/name；硬上限 1024 条 |
| **S2 调试控制** | `wait_for_event(events, timeout_ms, cancellable)` | 阻塞等 Breakpoint/Stepped/Paused/Resumed/Running/DebugStopped 事件之一；50ms 切片轮询 EventBus + 响应 ToolContext.cancelFlag；category=DbgControl（不弹 confirm 但写 audit） |
| **S3 写工具**（全部 category=Write + 5s confirm + audit） | `set_breakpoint(addr, type=software\|hardware)` | software 走 `Script::Debug::SetBreakpoint`；hardware 走 `SetHardwareBreakpoint`（最多 4 个，超限 x64dbg 自己拒） |
|  | `remove_breakpoint(addr)` | 软硬都试一遍删除，返回 `{software_removed, hardware_removed}` |
|  | `step_in(timeout_ms=30000)` | `DbgCmdExecDirect("StepInto")` + 私有 waitForStop（50ms 切片三路轮询 Paused/Breakpoint/Stepped + cancelFlag + `!DbgIsDebugging()` 检查） |
|  | `step_over(timeout_ms=30000)` | 同上，`StepOver` |
|  | `run_until(addr, timeout_ms=30000)` | `DbgCmdExecDirect("bp 0x.., ss")` 装 one-shot 断点 + `run` + waitForStop；timeout 路径兜底 `DeleteBreakpoint` 清理 |
|  | `run_dbg_command(command)` | 命令逃生口；首 token（按空白/逗号切）小写后查 15 token 白名单：`bp/bpc/bphwc/bpd/bpe` + `run/stepinto/stepover/stepout/pause` + `db/dw/dd/dq`；非白名单直接 deny + warn |
| **S4 数据写**（全部 category=Write + 5s confirm + audit） | `patch_memory(addr, bytes_hex)` | 写 hex 字节流；接受 "DE AD BE EF" / "deadbeef" / "DE,AD,BE,EF"；4 KB 上限；写前按 4 KB 步进 + 末字节做 `DbgMemIsValidReadPtr` 越界检查；`DbgMemWrite` 失败时报具体 VA |
|  | `set_register(name, value)` | 写 GPR / DR / EFLAGS / Cxx 别名；名表 90+ 条（含 R8B/R9W/SIL/SPL 等子寄存器；x86/x64 条件编译）；按 byteWidth 校验 value 范围（写 AL 超 0xFF 直接拒）；XMM/YMM/MXCSR/FPU 不支持 |
|  | `write_string(addr, value, encoding=utf8\|utf16le\|ascii)` | 默认 utf8；ascii 拒绝 >0x7F 字节避免静默 mojibake；utf16le 先 `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` 解码再按 2 字节 LE 输出；自动追加正确长度的 \0 终止；编码后硬上限 8 KB |

### ToolPolicy 三档（S3）

| Category | UI confirm | Audit | 适用 |
|---|---|---|---|
| `Read` | 否 | 否 | 全部读工具（基础读取 / 静态分析 / 动态上下文 / eval_expression / list_breakpoints） |
| `DbgControl` | 否 | begin/end | `wait_for_event` 等"占用执行流但不改状态"工具 |
| `Write` | **5s 倒计时模态** | begin/end + confirmed/denied_by_user/denied_no_ui | 全部写工具（断点 / 单步 / run_until / run_dbg_command） |

#### Write 工具 5s confirm（S3-C）

- `ToolConfirmDialog`：模态 QDialog，5 秒倒计时；"允许"按钮初始 disabled，文本 `允许 (Ns)`，QTimer 每秒 -1，归零后启用并去掉计数
- `denyButton_->setDefault(true)` → ESC/Enter **默认拒绝**；安全为先
- 工具运行在 worker 线程时通过 `QMetaObject::invokeMethod(app, lambda, BlockingQueuedConnection)` 切到 GUI 线程并阻塞等返回
- 无 `QApplication` 时安全 deny + 写 `phase=denied_no_ui`
- 参数 JSON 在等宽字体 dark 主题块内展示，高级用户可审

#### Write 工具审计日志（S3-B）

- 路径：`%APPDATA%\x64dbg-ai-plugin\logs\write_audit.log`
- 独立 spdlog logger `x64dbg-ai-audit`；rotating 4 MB × 10；pattern `%v`（纯 JSON 一行一条）；`flush_on(info)` 保证即时落盘
- JSON 字段：`ts(ms epoch) / tool / category / args / phase / sha / session` + 终态 `ok / error / data_snippet / elapsed_ms`
- phase ∈ `{begin, end, confirmed, denied_by_user, denied_no_ui}`
- 便于 grep/jq 复盘 agent 行为：`type write_audit.log | jq 'select(.phase=="denied_by_user")'`

### Agent loop

- `AgentLoop.run` 同步阻塞循环；外层 `AgentWorker` 用 `QtConcurrent::run` 跑后台线程
- 每轮 `provider.streamChat(messages, tools)`：
  - 文本增量 → 助手气泡
  - `reasoning_content` 增量 → ReasoningBlock 折叠面板
  - `tool_calls` 增量按 `delta.tool_calls[index]` 分组累积 `id / name / arguments`
- 本轮结束若 `tool_calls.empty()` → 完成；否则按序 `dispatch` 每个工具，结果 `role=tool, tool_call_id=...` 写回 `messages`，进入下一轮
- 安全上限：`max_iter = 20`（预设可调，1–50）；每工具 64 KB 硬截断；写类工具本批未开放
- 全部工具调用进 `plugin.log`

### Provider 兼容
| Provider | function calling | 行为 |
|---|---|---|
| DeepSeek | ✅ | 真正多轮 agent；reasoner 思考链通过 reasoning_content 通道 |
| Copilot | ⚠️ 不稳定 | `AgentLoop` 内部用空 tools 列表降级单轮 |

### UI 反馈
- **ToolCallCard**（`src/ui/tool_call_card.cpp`）：每个 tool_call 一张折叠卡片
  - pending（灰）→ running（蓝，显示 args）→ done（绿，显示耗时 + truncated 标识）/ error（红）
  - 默认折叠，标题如 `▶ read_memory  (12 ms)`
- **ReasoningBlock**（M4.6 Reasoning UI）：thinking 模型独立折叠面板，灰色等宽字体，标题 `▶ 思考过程 (N)` 显当前字符数

---

## 12. Agent 预设管理（M4.6a / M4.6e）

### 出厂预设（5 个，readonly）

| ID | 名字 | 用途 |
|---|---|---|
| `freeform` | 自由对话 | 默认；空 systemPrompt + 全工具，`showInContextMenu=false` |
| `analyze-function` | 分析当前函数 | 围绕 RIP/EIP 所在函数整体行为分析；含 v4 强约束 |
| `who-calls-here` | 谁调用了这里 | 重点排查调用方：xref + 调用栈 + trace + 调用点反汇编 |
| `string-api-context` | 字符串与 API 关联 | 字符串引用与 API 调用聚类（crypto/net/file/anti-debug） |
| `trace-summary` | Trace 概要 | 配合 Trace 子系统：trace_query 起步 + locate_api_callers/get_disasm 佐证 |

所有出厂预设 `readonly=true`，systemPrompt 末尾强制 `OUTPUT LANGUAGE RULE`（必须 zh-CN，保留代码/地址/寄存器/指令原文）。

#### v4 evidence rule（schemaVersion 4，D-03）

针对 fx_log1.txt 暴露的"AI 凭空推断调用关系 + 没代入入参演算"问题，`analyze-function` / `who-calls-here` / `string-api-context` 三个预设新增两条强约束：

- **EVIDENCE RULE**（三个都有）：任何关于地址 / 函数体 / 调用关系 / 数据布局的断言必须由本会话**实际做过的工具调用**支撑；未读过的标"推测"或不说。禁止凭"看起来像"虚构调用边。
- **CONCRETE INPUT RULE**（仅 `analyze-function`）：user prompt 或初始 disasm 上下文有具体入参值时，必须**代入逐步演算并报告结果**，不能只给通用算法描述。

### 预设编辑器（M4.6e PresetEditorDialog）

入口：`AssistantPanel` 顶部「预设管理」按钮 → modal 1000×660。

| 区域 | 内容 |
|---|---|
| 左 | QListWidget 列出全部预设；🔒 前缀 = readonly |
| 右 | QFormLayout 字段：id（只读）/ name / desc / provider（默认/DeepSeek/Copilot）/ model / maxIter（1–50）/ temperature（0.0–1.5）/ showInContextMenu / systemPrompt（多行）/ userTemplate（多行）/ enabledTools（QListWidget 复选 + 全选/全清） |
| 底 | 新建（QUuid 短 id）/ 复制副本 / 删除 / 恢复出厂 / 保存 / 关闭 |

特性：
- **readonly 字段全 readOnly + 禁 saveBtn**；仍可删除/复制（复制出的是用户预设可改）
- **dirty 跟踪**：切换列表项 / 关闭前若有未保存改动，弹"放弃修改？"确认
- **enabledTools 空集 = 全部**：UI 全勾时自动归一为空集，AgentRunRequest 透传时空 tools → ToolRegistry 全量
- **恢复出厂**温和语义：保留所有 `readonly=false` 用户预设，仅覆盖 readonly 那 5 项
- 关闭时 `emit changed()` → AssistantPanel 自动 `PresetStore::load() + 重建 Agent 下拉 + rebuildDisasmAiSubmenu() + activePreset 兜底`

### 持久化

`%APPDATA%\x64dbg-ai-plugin\agent_presets.json`：

```json
{
  "schemaVersion": 8,
  "presets": [{ "id": "...", "name": "...", "systemPrompt": "...", "userTemplate": "...",
                "enabledTools": ["..."], "maxIter": 20, "temperature": 0.2,
                "provider": "deepseek", "model": "", "showInContextMenu": true, "readonly": true }]
}
```

- 启动时 `diskSchema < kPresetSchemaVersion(=8)`：用新版 defaults 覆盖所有 readonly；用户预设保留
- 保存：`rename(.tmp → final)`；rename Access Denied（avast/Defender 抢锁）时 3 次重试 50 ms 间隔 + 原地 ofstream 覆写 fallback

---

## 13. 反汇编 AI ▶ 子菜单（M4.6f）

x64dbg 右键反汇编窗口 → `AI ▶`，子菜单动态列出所有 `showInContextMenu=true` 的预设。

### 实现要点
- `plugin/plugin_menus.cpp` `_plugin_menuadd("AI")` 建子菜单；entryId 静态池 `2000–2099`
- `map<entryId, presetId>`（mutex 保护）维护映射
- 预设变更 → `_plugin_menuclear` + 重填（保留 entryId 池上限以防溢出）
- 命中：`handleMenuEntry` 找到 presetId → `AssistantPanel::runPresetById(presetId, QString())`（user prompt 为空，纯模板驱动）
- 暴露 `rebuildDisasmAiSubmenu()` 给 PresetEditorDialog 保存后回调；启动时主动 `PresetStore::load()` 后挂载一次

### 与 ChatView 输入框关系
- 输入框回车始终走当前 `activePresetId_`（顶部 Agent 下拉显示）
- 右键子菜单走对应预设；不改变 activePresetId_
- preset.enabledTools 优先；顶部"工具菜单"仅做展示

---

## 14. 思考过程折叠面板（M4.6 Reasoning UI）

DeepSeek `deepseek-reasoner` 等 thinking 模型返回的 `reasoning_content`（CoT）独立显示。

### UI
- 每条助手消息上方挂一个 `ReasoningBlock`：QToolButton 折叠按钮 + 灰色等宽 QLabel
- 标题：流式过程 `▶ 思考过程 (1287)`（动态显示当前字符数）；完成后保留
- 默认折叠；点击展开看完整思考链
- 与下方助手气泡完全分离的 widget（QScrollArea + VBox 子控件流，M4.6c ChatView 重构方案 A）

### 协议
- `IChatProvider` 扩展 `ChatStreamCallbacks.onReasoningDelta(text)` + `ChatMessage.reasoningContent`
- `AgentLoop` 累积 reasoning + 透传 `cb.onAssistantReasoningDelta`
- `AgentWorker` 新增 `assistantReasoningDelta(QString)` 信号 → invokeMethod 投递主线程
- ChatView 新增 `appendAssistantReasoningDelta` / `finalizeAssistantMessage` 同时收尾 `streamingBubble_` 和 `streamingReasoning_`

### 关键约束
- **DeepSeek thinking 模型回传上一轮 reasoning_content**：M-1 (2026-05-24) 实测两种形式 deepseek-reasoner 都接受 HTTP 200；保留回传以兼容未来协议收紧 & 利于 UI 折叠展示
- 持久化到 SessionStore 时与 content 分字段保存
