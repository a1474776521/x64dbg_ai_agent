# 架构

## 1. 总体结构

```
┌────────────────────────────────────────────────────────────────┐
│                       x64dbg.exe (主进程)                       │
│  ┌──────────┐  ┌────────────────────────────────────────────┐  │
│  │ Bridge   │  │      x64dbg_ai_plugin.dp64                 │  │
│  │ (GUI/Dbg)│←→│  plugin/    入口、CB_* 回调、AI ▶ 子菜单    │  │
│  └──────────┘  │  ai/        IChatProvider + Agent loop +    │  │
│                │             ToolRegistry + 预设管理         │  │
│                │  ai/tools/  63 个工具（39 只读 + 5 控制 +    │  │
│                │             19 写）按 8 文件分组             │  │
│                │  debugger/  反汇编上下文采集                 │  │
│                │  locator/   启发式扫描器                     │  │
│                │  trace/     录制器 + 调用图 + 栈采样          │  │
│                │  storage/   sqlite（按 SHA 分库）+ 跨库浏览  │  │
│                │  ui/        AssistantPanel + ChatView +      │  │
│                │             ToolCallCard + 预设编辑器        │  │
│                │  util/      DPAPI / hash / paths / log      │  │
│                └────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘
        │                                       │
        ↓ HTTPS                                 ↓ HTTPS
   api.githubcopilot.com               api.deepseek.com
   models.github.ai (embedding)
```

插件以单 DLL 形式被 x64dbg 加载，导出 `pluginit/plugsetup/plugstop` 等 SDK 接口。所有依赖（Qt5、cpr、libcurl、OpenSSL、sqlite3、sqlite-vec、spdlog、nlohmann-json）走 vcpkg `*-windows-static-md` 静态链接，避免与 x64dbg 自带运行时冲突。

## 2. 模块分层

| 层 | 目录 | 职责 | 关键文件 |
|---|---|---|---|
| 入口 | `src/plugin/` | x64dbg 回调路由、菜单注册（含动态 AI ▶ 子菜单） | `plugin_main.cpp`、`plugin_callbacks.cpp`、`plugin_menus.cpp` |
| AI | `src/ai/` | LLM provider 抽象、OAuth、embedding、SSE、**Agent loop / 工具注册（63 个）/ 预设管理（15 出厂）/ usage 解析（G-2）** | `chat_provider.h`（UsageInfo + onUsage）、`copilot_*.cpp`、`deepseek_*.cpp`、`provider_manager.cpp`、`embedding_client.cpp`、`sse_parser.cpp`、`agent_loop.cpp`、`agent_worker.cpp`（`usageUpdated` signal）、`agent_preset.cpp`、`preset_store.cpp`、`tools/*.cpp`（8 分组文件）|
| 调试 | `src/debugger/` | 反汇编片段采集 / 选区识别 | `disasm_context.cpp` |
| 定位器 | `src/locator/` | API / 字符串 / 特征码 / 关键词扩展 | `locator_engine.cpp`、`api_scanner.cpp` 等 |
| Trace | `src/trace/` | 步进录制 + 调用图 + 反向栈采样 | `trace_recorder.cpp`、`call_graph.cpp`、`callstack_tracer.cpp` |
| 存储 | `src/storage/` | sqlite + sqlite-vec + 跨 DB 只读浏览 | `session_store.cpp`、`project_context.cpp`、`project_browser.cpp` |
| UI | `src/ui/` | Qt 主面板 + 各功能对话框 + **聊天流子控件 + 预设编辑器（QTreeWidget 三态）+ 工具列表对话框** | `assistant_panel.cpp`（`agentStatusLabel_` G-2 cache 拼接 + `usageUpdated` handler）、`chat_view.cpp`、`tool_call_card.cpp`、`preset_editor_dialog.cpp`、`tools_list_dialog.cpp`、`*_dialog.cpp` |
| 工具 | `src/util/` | DPAPI、SHA256、路径、配置、日志 | `secret_store.cpp`、`hashing.cpp`、`paths.cpp`、`config.cpp`、`logging.cpp` |

依赖方向严格自上而下：`ui → {ai, storage, trace, locator, debugger} → util`。任何下层不依赖 ui/Qt。

## 3. 线程模型

x64dbg 内部有三类线程，插件必须明确区分：

| 线程 | 来源 | 在 plugin 里做什么 | 注意事项 |
|---|---|---|---|
| **GUI 主线程** | x64dbg Qt 主线程 | 所有 UI 创建/更新；DbgCmdExec 也走它 | Qt widget 操作只能在这里 |
| **调试事件线程** | bridge → x64dbg 的 debug 子线程 | `CB_INITDEBUG / CB_STOPDEBUG / CB_BREAKPOINT / CB_MENUENTRY` | **回调里不能直接动 UI**；要 `QMetaObject::invokeMethod(... QueuedConnection)` |
| **后台 worker** | 插件自建（`QtConcurrent::run`） | HTTP 请求、embedding 计算、SHA256、模型列表拉取 | 结果回主线程更新 UI 也用 invokeMethod |
| **AgentWorker 线程**（M4） | `QtConcurrent::run` 包裹 `AgentLoop.run` | 整个 agent 多轮循环（含 streamChat 阻塞 + ToolRegistry.dispatch 同步调 SDK） | 所有 callback（onAssistantDelta / onToolStart / onToolReport 等）通过 `QMetaObject::invokeMethod(panel, ..., Qt::QueuedConnection)` 转发到主线程；loop 不感知 Qt |

典型一次 AI 分析的线程切换：

```
菜单点击(调试线程) ─invokeMethod→ 主线程组装 prompt
   │
   └─QtConcurrent::run→ worker 调 chat_->stream(...) (libcurl 阻塞)
                                │
                                └─SSE 每行 onDelta(...) ─invokeMethod→ 主线程 chat_->appendAssistantDelta
                                                                         │
                                                                         └─pendingDelta_ + 16ms QTimer
                                                                                                    └─ flushPendingDelta → insertText
```

## 4. 数据流

### 4.1 反汇编 AI 分析

```
DisasmContext (32 行) ──╮
                        ├──→ EmbeddingClient.embed → SessionStore.addChunk (RAG 写入)
                        │
                        └──→ buildRagAugmentedPrompt
                                   │
                                   ├─ EmbeddingClient.embed(user)
                                   ├─ SessionStore.searchSimilar(qEmb, k=4)
                                   └─ 拼接 top-K + 原 prompt → IChatProvider.stream(...)
                                                                       │
                                                                       └─ ChatView 增量渲染
                                                                       └─ SessionStore.appendMessage(role,content) ×2
```

### 4.2 调用链追溯（正向 trace 模式）

```
TraceDialog.start
   │
   ├─ TraceRecorder.setMode(Targeted/GlobalActive/Passive)
   ├─ DbgCmdExec("bp <addr>")  (Targeted)
   │
   └─ cbBreakpoint(调试线程)
       │
       ├─ CallStackTracer 分发（M3.5）
       └─ TraceRecorder.onBreakpoint
            │
            ├─ 记录 entrySp_/entryModName_/stepsSinceStart_
            ├─ DbgCmdExec("TraceIntoConditional 0")
            └─ DbgCmdExec("run")        ← 必须显式重启，否则 trace 不动
                   │
                   └─ cbTraceExecute(每步) → push CallNode / pop on return
                                                  │
                                                  └─ buildView → 折叠 → TreeView
```

### 4.3 调用栈反向采样（M3.5）

适用于 trace 跟不下去的系统叶子 API（如 `ws2_32.send` 走系统调用门）。

```
CallStackTracer.armWith(addr, maxSamples)
   │
   └─ DbgCmdExec("bp <addr>")
            │
            └─ cbBreakpoint
                 │
                 └─ DbgFunctions()->GetCallStack(&cs)   ← 同步 RtlVirtualUnwind
                       │
                       ├─ 序列化 frames（comment / DbgGetLabelAt / mod+0xRVA）
                       ├─ FNV-1a 64 hash(addr,from,to)*N 去重
                       ├─ 命中数 +1，达 maxSamples → "bc <addr>" 自动拆
                       └─ DbgCmdExec("run") 继续
```

### 4.4 跨 DB 历史浏览（M3.6）

```
HistoryDialog.show
   │
   ├─ ProjectBrowser.listProjectDbs()  ← 扫 %APPDATA%/x64dbg-ai-plugin/projects/*.db
   │
   ├─ 点选 db → ProjectBrowser.listSessions(dbPath)   (sqlite URI mode=ro&immutable=1)
   │
   ├─ 点选 session → ProjectBrowser.listMessages(dbPath, id)
   │
   └─ [导入] → 遍历 messages 写入当前活动 SessionStore
                 → createSession("原标题（导入自 sha前12）", model)
                 → appendMessage * N
                 → emit imported() → AssistantPanel.refreshSessionPanel
```

### 4.5 Agent 多轮工具调用（M4 ★最新）

把"一次性 prompt"升级为 LLM 自主多步推理。LLM 可以选择"先调工具→拿结果→再调工具→…→最终回答"。

```
用户在 ChatView 输入 / 反汇编右键 AI ▶ {preset}
   │
   └─ AssistantPanel::runPresetById(presetId, userQuery)
        │
        ├─ PresetStore.findById        → AgentPreset (systemPrompt / userTemplate / enabledTools / maxIter / provider / model)
        ├─ DisasmContext + expandPresetTemplate(userTemplate, {{cip}}/{{module}}/{{selection}}/{{disasm}}/{{user}})
        ├─ ToolRegistry.listChatTools  → 按 preset.enabledTools 过滤
        ├─ 组装 AgentRunRequest (provider/model/messages/tools/maxIter)
        │
        └─ AgentWorker.start(req)   ← QtConcurrent::run 后台线程
              │
              └─ AgentLoop.run(req, cb)   for iter in 1..maxIter:
                    │
                    ├─ provider.streamChat(messages, tools)
                    │     │
                    │     └─ SSE delta：
                    │          - delta.content              → cb.onAssistantDelta            → ChatView 气泡增量追加
                    │          - delta.reasoning_content    → cb.onAssistantReasoningDelta   → ReasoningBlock 折叠面板
                    │          - delta.tool_calls[index]    → 增量累积 id / name / arguments
                    │          - usage (G-2，include_usage) → cb.onUsage(UsageInfo)          → AgentWorker.usageUpdated signal
                    │                                                                          → AssistantPanel.agentStatusLabel_ 末尾拼 cache=N%
                    │
                    ├─ 本轮结束 → cb.onAssistantMessage(完整 content + reasoning + toolCalls)
                    │              → 写入 req.messages（role=assistant；含 reasoning_content 用于下一轮回传）
                    │              → UI 预创建 ToolCallCard (pending 状态)
                    │
                    ├─ if toolCalls.empty() → 完成，break
                    │
                    └─ for tc in toolCalls:
                          ├─ cb.onToolStart(id, name, args)            → 卡片 setRunning
                          ├─ ToolRegistry.dispatch(name, args, ctx)    → 同步 SDK 调用
                          ├─ 截断 (单次 64 KB 硬上限)
                          ├─ cb.onToolReport(id, name, args, result, ok, error, ms, truncated)
                          │                                              → 卡片 setDone / setError
                          ├─ 写入 req.messages（role=tool, tool_call_id=id, content=resultJson）
                          └─ 持久化 SessionStore "[tool:name] resultJson"
```

工具实现完全无状态；返回硬截断为 `{truncated, original_bytes, max_bytes, preview}`。地址类参数全用 string，内部 `parseUInt64` 解析（兼容 `0x` 前缀 / 十进制）。

**DeepSeek thinking 模型 reasoning_content 回传**：`ChatMessage` 含 `reasoningContent` 字段，assistant 消息序列化时回传给下一轮。M-1 (2026-05-24) 实测 deepseek-reasoner 接受"回传"与"剥离"两种形式，本插件保留回传以兼容未来协议收紧并复用 UI 折叠面板。

**场景预设 PHASE 0 verdict gate（S9 后续方案 C / 2026-05-25）**：unpack-helper / malware-triage / anti-anti-debug 三个场景预设的 systemPrompt 头部嵌入"PHASE 0 自检段"——先用 2-3 个低成本工具判断样本是否符合本预设前提（如 unpack-helper 检测 packer signature），不符合则主动 `STOP` 并建议改用其他预设；符合才进入 PHASE 1 主流程。PHASE 0 与 PHASE 1 共享同一 LLM 上下文与 maxIter 预算（非 sub-agent，决策见 `decisions.md`）。预算硬上限：unpack-helper max 3 / malware-triage max 2 / anti-anti-debug max 2。

### 4.6 Prompt Cache 观测（G-2 / 2026-05-25）

让用户实时看到 prompt cache 命中率，验证 system prompt + tools schema 的英文化 / 顺序稳定策略是否生效。

```
provider.streamChat
   │
   └─ SSE 末尾 [DONE] 前的 usage chunk
        │ (DeepSeek: stream_options.include_usage=true → 末轮带 usage)
        │ (Copilot:  同 OpenAI 协议，include_usage=true)
        │
        └─ parseUsage(json) → UsageInfo{
             prompt, completion, total,
             cachedRead,            ← DeepSeek prompt_cache_hit_tokens / OpenAI prompt_tokens_details.cached_tokens / Anthropic cache_read_input_tokens
             cacheCreation,         ← Anthropic cache_creation_input_tokens
             reasoning              ← OpenAI completion_tokens_details.reasoning_tokens
           }
                 │
                 └─ AgentLoop.onUsage(UsageInfo)
                       │
                       └─ AgentWorker emit usageUpdated(UsageInfo)
                             │
                             ├─ spdlog [G-2 CACHE] input=N cached=N miss=N hit_ratio=N.N% completion=N reasoning=N cache_creation=N
                             └─ AssistantPanel slot:
                                    lastCacheStatus_ = "· input=N cache=N%"
                                    setActivePreset 重画 → agentStatusLabel_ = "[预设名] · input=N cache=N%"
                                    setToolTip(完整 usage 多行明细)
```

`UsageInfo::hitRatio()` 返回 `cachedRead / prompt`；`prompt == 0` 时返 -1，UI 显示 `cache=n/a`、日志写 `hit_ratio=n/a`（区分"cache 失效=0%"与"模型不暴露字段=n/a"）。Copilot 后端路由不同模型时字段命名不一，`parseUsage()` 三套字段兜底（K-24）。

## 5. 存储 Schema

每个被调试 EXE 一份独立 sqlite 文件：`%APPDATA%/x64dbg-ai-plugin/projects/<sha256>.db`。

```sql
CREATE TABLE sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  title TEXT NOT NULL,
  model TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  role TEXT NOT NULL,      -- user / assistant / system / tool
  content TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX idx_messages_session ON messages(session_id);

CREATE TABLE chunks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL,      -- asm / string / api / note ...
  va INTEGER NOT NULL DEFAULT 0,
  text TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX idx_chunks_va ON chunks(va);

-- sqlite-vec 虚拟表，rowid 与 chunks.id 对齐
CREATE VIRTUAL TABLE vec_chunks USING vec0(embedding float[1536]);
```

PRAGMA：`journal_mode=WAL`、`foreign_keys=ON`、`synchronous=NORMAL`。

### 5.1 预设存储（M4.6a）

Agent 预设保存为单一 JSON 文件，与 sqlite 库分离：

```
%APPDATA%\x64dbg-ai-plugin\agent_presets.json

{
  "schemaVersion": 14,
  "presets": [
    { "id": "...", "name": "...", "systemPrompt": "...",
      "userTemplate": "...", "enabledTools": ["..."],
      "maxIter": 20, "temperature": 0.2,
      "provider": "deepseek", "model": "",
      "showInContextMenu": true, "readonly": true,
      "tags": ["exploration", "triage"],          // S9 G-10 工具/预设标签
      "group": "exploration",                     // 分组（general/exploration/cracking/tracing/scenarios）
      "outputContract": "...",                    // 强约束输出格式（如 JSON schema 描述）
      "policyOverrides": { "write_memory": "deny" } // 预设级 ToolPolicy 覆盖
    },
    ...
  ]
}
```

- 出厂预设 **15 个**全部 `readonly=true`（general 3 / exploration 6 / cracking 2 / tracing 1 / scenarios 3），由 `defaultPresets()` 返回
- 启动时若磁盘 `schemaVersion < kPresetSchemaVersion (=14)`：用新版 defaults 覆盖所有 readonly，保留用户预设
- 保存路径：`rename(.tmp → final)`；rename Access Denied 时 3 次重试 + 原地 ofstream 覆写 fallback（avast / Windows Defender 抢锁）
- schema 历次升级触发因素：v3 加 provider/model；v4 D-03 evidence rule；v6-v10 各组预设引入；v13 S9 tags/policyOverrides；v14 S9 后续方案 C 新增 sample-triage + 三场景预设 PHASE 0 verdict gate

## 6. 关键设计决策

| 决策 | 原因 |
|---|---|
| 全静态 vcpkg `*-windows-static-md` | x64dbg 自带 Qt5 + MSVC 运行时；插件运行时不能再带一份 dll |
| 按 EXE SHA256 分库 | 每个目标程序的 RAG 上下文/会话完全隔离，避免污染 |
| `IChatProvider` 抽象 | M3.3 加 DeepSeek 几乎零成本；UI 不感知具体 provider |
| Copilot 必须伪装 VSCode | GitHub Copilot Chat API 只对 VSCode `client_id` 开放 |
| Embedding 走 GitHub Models（PAT） | Copilot OAuth token 没有 embedding 权限；DeepSeek 没有 embedding 接口 |
| 流式与普通请求分开超时 | 流式可能持续数分钟；普通请求 30 s 已足 |
| TraceRecorder 与 CallStackTracer 独立单例 | 两者都注册 CB_BREAKPOINT；统一在 TraceRecorder 内分发避免覆盖 |
| `bp` 命中后必须显式 `run` | x64dbg 在断点回调里执行 `TraceIntoConditional 0` 不会自动恢复 |
| CallStack 采样改用 `DbgFunctions()->GetCallStack` | 不需要先跑完一遍；命中瞬间的线程栈已包含全部 caller |
| ChatView 流式增量（M3.4）→ QScrollArea + VBox + 子控件流（M4.6c） | 原 setHtml 嵌套 table 卡死；M4 需要嵌入 ToolCallCard / ReasoningBlock 等真 QWidget，必须改 widget 流式布局 |
| 跨 DB 浏览用原生 sqlite3 URI ro 模式 | 不复用 SessionStore 避免触发 vec_chunks schema 检查 |
| QSS 仅作用于 AssistantPanel 子树 | 不污染 x64dbg 主界面其他 Qt 部件 |
| 中文 UI + 英文 system prompt | LLM 对英文 prompt 响应更稳定；中文显示更友好；预设 systemPrompt 末尾强制 "Please answer in Simplified Chinese." |
| 自实现 tool calling，不引 MCP | 范围可控；x64dbg SDK 全是同步 C 接口；MCP 协议层对本项目过重 |
| Agent 工具完全无状态 + 单次 64 KB 硬截断 | 防止 LLM 一次 read_memory 把 128 MB 内存喂进 context |
| AgentLoop 同步阻塞 + UI 端 QtConcurrent::run + invokeMethod QueuedConnection | loop 内部代码不感知 Qt 线程；所有 UI 回调统一在 AgentWorker 跨线程投递 |
| DeepSeek thinking 模型 reasoning_content 回传 | M-1 实测两种形式均 200；保留回传向下兼容 + 复用 UI 折叠面板 |
| Copilot 走 Agent 时自动降级单轮 | Copilot Chat API 不稳定支持 OpenAI function calling；用空 tools 列表退化为普通流式 |
| 5 出厂预设 readonly | 避免用户误改后无法恢复；UI 提供「复制为新预设」入口 |
| 反汇编右键 AI ▶ 子菜单动态枚举 | 用户可选择性把常用预设（如「分析当前函数」）暴露到右键，不堆塞菜单 |
| 写工具 5s 倒计时强制 confirm（S3）| 长会话疲劳下用户可能盲点 OK；5s 强制看一眼又不至于卡死流程；不开 autoApprove 白名单（决策详 `decisions.md`）|
| ToolPolicy 三档 Read/DbgControl/Write（S3）| `wait_for_event` / `step_*` / `run_*` 归 DbgControl：要 audit 不要 confirm，否则交互节奏崩溃 |
| 独立 write_audit.log（S3）| spdlog 多 logger，pattern 裸 `%v` → JSON 单行格式不被时间戳污染，grep/jq 友好 |
| label/comment 用 `text=""` 表删除（S6）| 不另开 delete_label / delete_comment，减少同义工具决策噪声 + 缩 tools schema 体积 |
| run_continue 默认 fire-and-forget（S6）| 长跑场景不浪费 agent loop iteration；LLM 显式用 wait_for_event 更精确 |
| get_cfg 只输出 Mermaid 不带 JSON（S7）| 节点 label 已编码 start/end/icount/terminal/icall；token 减半 |
| 不暴露 stack_pop（S8）| 真弹出破坏 ESP 一致性；显式 peek+set_register 让 agent 思考"要不要改栈" |
| anti-debug-bypass 合并入 anti-anti-debug（S8）| 单预设两阶段 workflow 比双预设跳转上下文连贯；后由 sample-triage 补"只诊断"生态位 |
| Tool description 中文化只做 UI（S9 G-9 A 档）| 保护 prompt cache（system prompt + tools schema 英文一致）；中文 token 占用 +30% 在 G-2 验证 cache 命中率前是盲改 |
| PresetEditor 工具列表用 QTreeWidget 三态（S9 G-10）| 63 工具单 ListWidget 平铺过载；整组勾选语义最自然 |
| Scenario 预设 inline PHASE 0 verdict gate（S9 后续方案 C）| 不上 sub-agent 框架：共享 prompt cache + 单轮对话上下文；解决"前提硬编码导致空转" |
| 流式 body 加 `stream_options.include_usage=true`（G-2）| OpenAI 协议标准方式拿 cache 字段；少数兼容端点可能 400（K-25）|
| Cache 状态拼 `agentStatusLabel_` 末尾（G-2）| 顶栏宽度有限避免新加 label；详细数据走 ToolTip |

## 7. 构建链路

```
vcpkg.json (manifest)
   │
   └─ vcpkg install (build_all.ps1 触发)
         │
         ├─ cpr / nlohmann_json / spdlog / openssl / sqlite3 / cmark
         └─ sqlite-vec 用 third_party 内 amalgamation，单独 add_library
              │
              └─ CMake target sqlite_vec
                    │
                    └─ x64dbg_ai_plugin（双架构两次构建）
                          ├─ -DCMAKE_BUILD_TYPE=Release
                          ├─ -DVCPKG_TARGET_TRIPLET=x64-windows-static-md / x86-windows-static-md
                          └─ -DCMAKE_TOOLCHAIN_FILE=vcpkg.cmake
```

产物：`build-x64\bin\Release\x64dbg_ai_plugin.dp64` + `build-x86\bin\Release\x64dbg_ai_plugin.dp32`。
`scripts\package_release.ps1` 打 zip 并按 `release\{x64,x32}\plugins\` 布局部署。
