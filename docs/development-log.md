# 开发日志：问题与解决方法

按里程碑顺序记录开发过程中遇到的实质性问题、根因分析、解决方案。
不记小语法错和琐碎调整，只留对后续维护有参考价值的内容。

---

## M1：骨架与基础接入

### 问题 1.1：x64dbg SDK 头文件 include 顺序
**现象**：`bridgemain.h` 与 Qt 头文件混用时出现 `min/max` 宏污染、`interface` 关键字冲突。

**解决**：
- 所有源文件先 include Qt，再 include `_plugins.h / bridgemain.h`
- 在 `bridgemain.h` 之前定义 `NOMINMAX` 与 `WIN32_LEAN_AND_MEAN`
- 把 SDK 头文件包装放在 `third_party/pluginsdk/`，统一通过 `#include "_plugins.h"` 入口

### 问题 1.2：Qt5 静态构建与 x64dbg 动态 Qt 冲突
**现象**：用 dynamic Qt 构建插件加载后，x64dbg 主界面 Qt 部件出现样式异常。

**解决**：vcpkg triplet 锁 `*-windows-static-md`（**static-md** 而非 static），让 MSVC 运行时跟 x64dbg 走同一份 vcruntime140.dll，但 Qt5 等其他依赖完全静态进 .dp64。

---

## M2：会话存储 + Copilot 接入

### 问题 2.1：sqlite-vec 在 vcpkg 中不存在
**现象**：`vcpkg install sqlite-vec` 失败。

**解决**：
- 用 v0.1.9 [amalgamation](https://github.com/asg017/sqlite-vec/releases) 单文件版
- 放 `third_party/sqlite-vec/`，CMake 里 `add_library(sqlite_vec STATIC ...)`
- 注意：必须 `#define SQLITE_CORE` 再 `#include <sqlite-vec.h>`，否则它会把所有 `sqlite3_*` 宏化为 `sqlite3_api` 间接调用（按 sqlite extension dll 模式），主进程链接报 unresolved external

### 问题 2.2：sqlite-vec 加载时机
**现象**：每个 `sqlite3_open` 后单独 `sqlite3_vec_init` 容易漏。

**解决**：用 `sqlite3_auto_extension(sqlite3_vec_init)` 注册为全局自动加载；首次注册后所有新连接都带上。封装 `ensureVecAutoExtension()` 用 `static bool registered`  去重。

### 问题 2.3：Copilot OAuth 流程
**现象**：用社区文档的 `client_id` 走 Device Flow 能换 OAuth token，但拿 token 调 `/v1/chat/completions` 返回 403。

**根因**：GitHub Copilot Chat API 对客户端身份做严格白名单，普通 OAuth App 不被允许。

**解决**：
- 伪装成 VSCode：`client_id = 01ab8ac9400c4e429b23`（VSCode Copilot 公开的）
- 每个请求加 header `Copilot-Integration-Id: vscode-chat`
- User-Agent 用 `GitHubCopilotChat/0.x.x`
- 注意：这是与 VSCode 共享的 client_id，凭据放 `%USERPROFILE%\.config\github-copilot\` 可与 gh / VSCode / OpenCode 共用

### 问题 2.4：libcurl TLS 证书撤销检查超时
**现象**：部分企业网 / 跨国网下 HTTPS 请求长时间卡住，最后 CURL 报 60。

**根因**：默认开启 CRL/OCSP 检查，撤销列表服务器不可达。

**解决**：cpr ssl options 加 `CURLSSLOPT_NO_REVOKE`；保留证书链校验本身。

---

## M3.0 / M3.1：UI 美化 + 启发式定位

### 问题 3.0.1：QSS 影响 x64dbg 主界面
**现象**：在 `qApp->setStyleSheet(...)` 应用主题后，x64dbg 自带 Qt 控件（反汇编窗口）配色全乱。

**解决**：
- QSS 只对 `AssistantPanel` 自身 `setStyleSheet`，作用域局限于其子树
- 子对话框（LocatorDialog / TraceDialog / HistoryDialog）构造时显式继承父 styleSheet
- 命名约定：`setObjectName("sectionHeader" / "loginStatusOk" / ...)` 让 QSS 用 ID 选择器

### 问题 3.0.2：lucide-icons SVG 在 Qt 里描边颜色不对
**现象**：lucide 默认 `stroke="currentColor"`，Qt 5.12 QSvgRenderer 不解析 `currentColor`。

**解决**：批量把 SVG 里的 `stroke="currentColor"` 替换为固定 `#cccccc`（深色主题专用）。

### 问题 3.1.1：`DbgGetExportFromName` 大小写敏感
**现象**：用户输入 `Send` 取不到，必须 `send`。

**解决**：定位器输入框统一 `toLower()`；同时尝试加常见前缀（`_send`、`__imp_send`）。

### 问题 3.1.2：LocatorDialog 缺关键字输入入口（M3.1.k 补丁）
**现象**：`LocatorDialog` 顶部只有 3 个 scanner checkbox + 自动写 RAG，没有任何让用户输入"我关心的字符串/API/字节模式"的入口。三个 scanner 全用内置词表/特征码，意味着用户无法精准定位自己关心的目标（如 license/error 子串、特定导入名、`DE AD BE EF` 魔数）。

**解决**：
- 三个 scanner 各自 `Options` / `scan(...)` 增加 `userKeywords` 字段
- `StringScanner`：keyword 大小写不敏感 substring 命中 +40 分，`category` 空则改为 `user-keyword`，evidence 追加 `[kw:xxx]`
- `ApiScanner`：内置敏感表未命中时，对归一化 IAT 名 + 原始 IAT 名（小写）做 substring 匹配；命中收录，`category="user-keyword"`，score=70
- `PatternScanner`：新增 `looksLikeHexPattern` + `parseHexPattern`（支持 `??` / `?` 整字节通配），区分两种形态：hex pattern 走 mask memmem；普通字符串同时按 ASCII 与 UTF-16LE 扫描所有节
- `LocatorEngine::Options::userKeywords` 统一传递；API/String 在 engine 内部小写化，Pattern 保留原始用于 hex 判别
- UI：`LocatorDialog` 增加 `关键字: <QLineEdit>` 行，QSettings 持久化（`x64dbg-ai-plugin/locator/userKeywords`）
- 分隔符策略：`,` / `;` / 换行 作为主分隔；**空格保留**（因为 hex pattern 形如 `"DE AD BE EF"` 必须保留空格）。需要输入多个独立关键词时用逗号分隔

**坑**：Qt 5.12 没有 `Qt::SkipEmptyParts`（5.14 才加），编译报 `C2039`。改用旧式 `QString::SkipEmptyParts`。

---

## M3.2 / M3.5：调用链追溯

### 问题 3.2.1：x64dbg trace 文件被动解析不可靠
**现象**：原计划解析 x64dbg 保存的 `.trace64` 二进制文件被动重建调用链。文件格式无文档、版本间不稳。

**解决**：放弃文件解析，改为**主动 trace + cbTraceExecute 回调**实时构图。优点：实时；缺点：trace 速度受插件回调拖慢。

### 问题 3.2.2：调用图节点爆炸
**现象**：实际程序的调用图动辄数万节点，TreeView 卡。

**解决**：分两层做折叠
1. 兄弟节点合并：同 (from→to) 合一个，hits 累加
2. 连续系统模块兄弟聚合：`ucrtbase / msvcrt / kernelbase / ...` 等约 30 个模块在同一层连续出现时合并为灰色节点（如 "ucrtbase × 7"）
3. 默认按 hits 降序排，叶子模块名截断

### 问题 3.2.3：Targeted Trace 命中后不动
**现象**：在目标函数下 `bp`，命中回调里执行 `TraceIntoConditional 0`，然而调试器停在断点处不前进。

**根因**：x64dbg 的 trace 命令需要调试器处于 running 状态；命中断点后状态已是 paused，trace 不会自动恢复。

**解决**：`onBreakpoint` 内执行 `TraceIntoConditional 0` 之后**追加 `DbgCmdExec("run")`**，显式恢复运行。

定位过程：从 spdlog 看到"trace 命令已发，但没有任何 cbTraceExecute 触发"；查 x64dbg 源码确认 trace 命令的前置条件是 running 状态。

### 问题 3.2.4：Targeted Trace 在某些函数永不"返回"
**现象**：函数明明已执行完，trace 却一直延续到调用方继续步进，最终采集数十万步后超时。

**根因**：原判定逻辑 `funcEnd != 0 && curEip >= funcEnd` 依赖反汇编算函数末尾；对编译器优化过的 PGO/tail-call/handler 拆分函数算不准。

**解决**：当 `funcEnd == 0`（拿不到）时改用纯 SP 判定：
- 记 `entrySp_ = ev.sp`（命中瞬间 SP）
- 每步检查 `stepsSinceStart_ >= 2 && curSp > entrySp_`（**严格抬高**）
- 之所以要 `>= 2`：第一步是 push return-addr 的 call，SP 反而会降到 entrySp-8

注意：`ev.sp` 是**指令执行前** SP；call 前 SP 高，ret 前 SP 低（=entrySp-8）。

### 问题 3.2.5：ws2_32.send 跟丢
**现象**：在 `ws2_32.send` 入口下 Targeted Trace。HIT → `bphc` → step #1 → 之后完全没有 cbTraceExecute → 6 分钟超时后 plugstop。

**根因**：`send` 是系统叶子 API，最终走 Windows 系统调用门进内核态。x64dbg 在 syscall 处 trace 跟丢，且不抛错。

**解决**：催生了 **M3.5 CallStack 反向采样**——既然顺着跟不下去，那就在叶子 API 命中时**回头看**线程栈上的 caller chain。

### 问题 3.5.1：GetCallStack API 在哪
**现象**：x64dbg SDK 头里 `_dbgfunctions.h` 暗藏一堆未文档化的函数指针。

**定位**：
- `_dbgfunctions.h` L18-30 定义 `DBGCALLSTACKENTRY { addr, from, to, comment[MAX_COMMENT_SIZE] }` + `DBGCALLSTACK { total, entries }`
- L219 / L260 / L274 分别是 `GetCallStack / GetCallStackEx / GetCallStackByThread`
- 调用：`DbgFunctions()->GetCallStack(&cs)`，结束后用 `BridgeFree(cs.entries)` 释放

### 问题 3.5.2：CallStackTracer 与 TraceRecorder 都注册 CB_BREAKPOINT 互相覆盖
**现象**：注册第二个 cbBreakpoint 后第一个失效。

**根因**：x64dbg 的 plugsetup 注册回调时只保留最后一个同类型回调。

**解决**：
- TraceRecorder 持有 `cbBreakpoint` 单例
- CallStackTracer 不再独立注册，TraceRecorder::cbBreakpoint 内部按 armed 状态分发：
  ```cpp
  if (CallStackTracer::instance().isArmed())
      CallStackTracer::instance().onBreakpoint(info);
  // 然后再处理 trace_recorder 自己的逻辑
  ```

### 问题 3.5.3：CallStack 采样次次相同
**现象**：第一次设计是命中后 `bc` 一次只采一份。改成允许多次后发现 32 次采样全是同一条 frames。

**根因**：同一执行路径上的 caller chain 是确定的；只有不同代码路径调到 `send` 才会出现不同 stacks。

**解决**：保留多次采样但加 FNV-1a hash (addr,from,to)*N 去重，UI 展示 `unique / total`。实际意义：发现"有几条不同的业务路径都会调到这个 API"。

### 问题 3.5.4：CallStack 模式断点必须自己拆
**现象**：armed 状态下达到 maxSamples 后断点继续命中，干扰用户。

**解决**：达上限时 `DbgCmdExec("bc <addr>")` 自动拆，把 armed_ 状态置 false。

---

## M3.3：DeepSeek 接入

### 问题 3.3.1：流式 HTTP 超时
**现象**：DeepSeek reasoner 模型答复时间长（思考链 30 s+），libcurl 默认 timeout 30 s，每次都断流。

**解决**：HTTP options 分离两套：
- `timeoutMs = 30000`（普通请求 / 模型列表）
- `streamTimeoutMs = 600000` + `streamLowSpeedSec = 30`（流式：10 分钟总超时，30 秒无数据视为断）

### 问题 3.3.2：reasoner 的 reasoning_content 字段
**现象**：deepseek-reasoner 返回 SSE 里同时有 `delta.content` 和 `delta.reasoning_content`，原 SSE parser 只取 content。

**解决**：`sse_parser.cpp` 解析时两字段都取，按 `[思考]` `[正文]` 拼接通过 onDelta 流出，UI 端不区分。

---

## M3.4？：聊天闪烁/黑屏（**最严重的一次**）

### 现象
长答复流式响应时聊天区疯狂闪烁，最终 1-2 秒黑屏，期间整个 x64dbg UI 假死。

### 根因定位
1. 起初怀疑 GPU 驱动 / Qt 渲染 bug，无果
2. 加日志发现：每次 SSE delta 都触发了一次完整 `rerender()` → `setHtml(entire transcript)`
3. ChatView 的气泡是嵌套 `<table>` 实现的——QTextDocument 对嵌套 table 的 layout 是 O(总长度)
4. 当 transcript 累计到 5 KB+，每秒 30 次 setHtml 直接把 GUI 线程填满

### 方案选型
- 方案 A：流式期间不重排，用 `QTextCursor::insertText` 在末尾增量追加 → 选这个
- 方案 B：换 QWebEngineView → 太重，引入新依赖
- 方案 C：把气泡改成纯 div → 仍要 setHtml，治标不治本

### 实现要点
1. **流式起点**：`appendAssistantHeader` 一次 setHtml 出空气泡 + 取 `streamCursor_(End)`
2. **delta 入队 + 节流**：`pendingDelta_ += text; flushTimer_->start(16)`（16 ms 单次定时器）
3. **flush**：`streamCursor_.insertText(local)` 增量追加
4. **finalize**：停 timer + flush 残余 + setStreamCursor null + 一次 rerender 补"助手" footer
5. **QTextCursor 失效问题**：任何 setHtml 前必须清 `streamCursor_`；外部 rerender 路径（`appendUserMessage / appendSystemNote / clearTranscript / 切会话`）必须先 finalize 或显式重置流式状态

### 配套修复
- 自动滚动改"仅贴底才滚"：监听 `valueChanged` 维护 `userAtBottom_`（4 px 容差），`programmaticScroll_` 屏蔽自己 `setValue` 的反馈
- 流式期间 `setInputEnabled(false)`：禁止用户在前一条还没结束时发新请求（会破坏 streamCursor 状态）

### 教训
- 富文本控件的 layout 复杂度要算清楚，嵌套 table 是大忌
- 流式 UI 一律走"增量追加 + 节流"模式，不要每个 chunk 都全量重渲

---

## M3.6：跨 DB 会话浏览器

### 触发场景
用户反馈"昨天的会话今天加载不了，同一个目标调试进程"。排查发现：
```
%APPDATA%\x64dbg-ai-plugin\projects\
  0ccb29...db  (今天，0 KB)
  f368fd...db  (今天另一次启动，0 KB)
  bfad81...db  (昨天，288 KB ← 用户要找的)
  99a7d8...db  (51 KB)
  b32989...db  (225 KB)
```
游戏客户端被自动更新，EXE 的 SHA256 变了 → DB 路径就变了。

### 设计决策
| 备选 | 评估 |
|---|---|
| 手动改 db 文件名救回 | 治标不治本，下次更新还会出问题 |
| 改 ProjectId 算法（EXE 文件名 + 导出表 hash） | 改动面大，老库要迁移，引入不确定性 |
| **加 UI 跨 DB 浏览/导入**（选这个） | 非破坏；保留 SHA 隔离不变；老库零修改 |
| 配置文件手动绑定 ProjectId 别名 | UX 差；未来再考虑 |

### 实现要点
1. **`ProjectBrowser`**（`src/storage/project_browser.{h,cpp}`）独立模块：
   - `listProjectDbs()` 扫 `projects/*.db`，返回 `{sha, sizeBytes, mtime}` 按 mtime 降序
   - `listSessions(dbPath)` / `listMessages(dbPath, id)` 只读访问任意路径
   - 不复用 SessionStore：避免触发 sqlite-vec schema 检查、`vec_chunks` 不存在的老库会失败
2. **只读打开方式**：sqlite URI `file:<path>?mode=ro&immutable=1`
   - `mode=ro`：绝对不写
   - `immutable=1`：告诉 sqlite 这个文件不会被其他进程改，跳过 WAL 检查
3. **`HistoryDialog`**（`src/ui/history_dialog.{h,cpp}`）：
   - 三栏 splitter（DB 列表 / sessions / messages 预览）
   - 当前活动 db 用 ● + bold 标记
   - 消息预览只用 `<pre>` + `toHtmlEscaped`：避开 ChatView 那种嵌套 table，防止重蹈 M3.4 覆辙
4. **导入语义**：
   - 只拷 messages，不带 chunks/embeddings → 不污染当前 RAG
   - 新会话标题加 `（导入自 sha前12）` 来源标记
   - 导入完 `emit imported()`，AssistantPanel 收到自动 `refreshSessionPanel`
   - 对话框不关闭，方便连续导入

### 防呆
- 选中 db == 当前活动 db：按钮禁用，提示"已是当前项目"
- 当前未在调试：禁用，提示"无目标项目可导入"
- 老 db 无 sessions：导入按钮自动禁用

---

## 通用经验

### A. x64dbg 回调线程纪律
所有 `CB_*` 回调（CB_INITDEBUG / CB_STOPDEBUG / CB_BREAKPOINT / CB_MENUENTRY / CB_TRACEEXECUTE）来自 x64dbg 调试事件线程，**严禁直接动 Qt widget**。统一模式：

```cpp
QMetaObject::invokeMethod(qApp, [args]() {
    // 主线程操作
}, Qt::QueuedConnection);
```

### B. DbgCmdExec 必须主线程
`DbgCmdExec("run")` 等命令同样需要主线程。如果在 worker 线程触发要 marshall 回去。

### C. spdlog 末尾日志可能丢
spdlog 的 file sink 有缓冲；崩溃前最后几行可能未刷盘。调试时可在关键路径加 `spdlog::flush_on(spdlog::level::info)`，但日常不要全开（性能损耗）。

### D. trace_demo 必须 NOINLINE + /utf-8
- `__declspec(noinline)` 防止业务函数被内联——否则调用链消失
- `/utf-8` 防止源码里中文字符串编码混乱

### E. cpr 流式回调线程
cpr 的 write callback 在 libcurl IO 线程执行，跟主线程异步。回调里只做"把 chunk 投递到主线程"，不做解析（解析也可以在 IO 线程做，但要保证线程安全）。

### F. QTextCursor 生命周期
任何会触发 QTextDocument 重建的操作（setHtml / setPlainText / clear）之后，所有之前持有的 QTextCursor 立即失效，再用就 UB。流式渲染要严格守护此不变式。

---

## M4：Agent 多轮工具调用

把 M3 的"一次性 prompt"升级为 LLM 自主多步推理。子任务划分：M4.1–M4.5 = 后端（provider 扩展 + ToolRegistry + AgentLoop），M4.6a–M4.6f = UI 与持久化（预设系统 + ChatView 重构 + 反汇编子菜单 + Reasoning UI + 预设编辑器）。

### 问题 4.1：要不要走 MCP
**评估**：MCP（Model Context Protocol）是 Anthropic 推的工具调用标准，已有 SDK。
**决策**：不走。理由：
- x64dbg SDK 全是同步 C 接口，工具调用本身简单
- MCP 需要起 stdio/SSE server 子进程，给单 dll 插件加重
- 自实现 ToolRegistry 200 行就够，可控性高
**结论**：自实现 + 兼容 OpenAI function calling JSON schema（DeepSeek 直用，Copilot 自动降级）

### 问题 4.2：工具粒度
**早期方案**：粗粒度 `analyze_function(addr)` 一个工具搞定。
**问题**：LLM 不知道里面做了什么，无法控制资源；返回结果太大易爆 context。
**最终**：细粒度只读工具 12 个，全部无状态；返回硬截断 64 KB；LLM 自己组合。
**好处**：日志可读、可审计；写类工具暂不开放（防止 LLM 误操作目标进程）

### 问题 4.3：DeepSeek SSE tool_calls 增量协议
**现象**：DeepSeek 流式返回的 `delta.tool_calls` 字段不是完整对象数组，而是按 `index` 分组的增量片段；`id / function.name` 可能首块给，`function.arguments` 字符串分多块来。
**解决**：按 `delta.tool_calls[i].index` 维护累积器：
```cpp
toolCallAccum[index].id     += delta.id     (首次出现);
toolCallAccum[index].name   += delta.name   (首次出现);
toolCallAccum[index].args   += delta.args   (拼接每块);
```
本轮 SSE 完结后 JSON parse `args` 字符串得到最终 arguments object。

### 问题 4.4：assistant 携带 tool_calls 时 content=null
**坑点**：OpenAI 协议规定 assistant 消息带 `tool_calls` 时 `content` 必须是 null，不能是空字符串。
**症状**：传 `content=""` DeepSeek 返 HTTP 400。
**解决**：`ChatMessage` 序列化时检查：有 toolCalls 且 content 为空 → 输出 `"content": null`。

### 问题 4.5：tool 消息必须带 tool_call_id
**坑点**：role=tool 的消息没有 `tool_call_id` 字段就 400。
**解决**：ToolRegistry.dispatch 返回时把原 `tool_call.id` 透传回来，组装 `{role:tool, tool_call_id:id, content:resultJson}`。

### 问题 4.6：Copilot 走 agent 不稳定
**现象**：同 prompt 同 tools，Copilot 时而正常多轮，时而瞎编 Claude XML 风格 `<invoke>` 标签当文本输出。
**根因**：Copilot Chat API 对 OpenAI function calling 的兼容是 best-effort，模型/路由不固定。
**解决**：`AgentLoop.useTools()` 仅 provider=="deepseek" 时返 true；Copilot 强制空 tools 列表降级为普通流式。用户不感知（输入流程一样）。

### 问题 4.7：tools=[] 的兼容性
**现象**：DeepSeek 传 `tools: []` 偶发让模型瞎编 Claude XML。
**解决**：tools 为空时**根本不发送 `tools` 字段**（key 不存在），不是发送空数组。

### 问题 4.8：UI 线程与 agent 线程
**约束**：AgentLoop 内部不能感知 Qt；UI callback 必须主线程。
**方案**：`AgentWorker` 包一层 QObject，loop 内的 callback 通过 `QMetaObject::invokeMethod(this, [..](){ emit signal(); }, Qt::QueuedConnection)` 投递到 worker 自己（worker 也在 main thread），再 emit 信号给 panel。所有 connect 用 `Qt::AutoConnection`（同线程直连）。
**好处**：AgentLoop 是纯 C++ 模块，可单测；线程切换集中在 AgentWorker。

### 问题 4.9：ChatView 嵌入 ToolCallCard 失败
**现象**：原 ChatView 用 QTextDocument + setHtml 渲染气泡。要嵌 ToolCallCard 这种真 QWidget 必须用 `QTextDocument::setDocumentMargin` + 自定义 ObjectInterface，复杂且渲染抽风。
**决策**：**M4.6c ChatView 整体重构（方案 A）**：
- 从 QTextEdit 改成 `QScrollArea + QWidget(VBox 容器)`
- 每条消息一个独立子 widget（气泡 / 系统提示 / ToolCallCard / ReasoningBlock）
- 流式增量：`streamingBubble_` 是独立 QLabel，`setText(text + delta)` 追加（QLabel 富文本支持有限但够用）
- 滚动策略复用 M3.4 那套（userAtBottom_ + programmaticScroll_）
**收益**：嵌任何 QWidget 都自然；不再受 setHtml 重排卡顿影响

### 问题 4.10：预设 JSON 写入 Access Denied
**现象**：保存 `agent_presets.json` 时偶发 `rename(.tmp → final)` 抛 `Access is denied`。
**根因**：avast / Windows Defender 在文件写入完成瞬间打开扫描，持锁几十 ms。
**解决**：rename 失败时 3 次重试，每次 sleep 50 ms；仍失败则 fallback 用 `ofstream` 原地覆写（不再走 rename）。日志记录所有失败/重试。

### 问题 4.11：预设 schema 升级
**场景**：M4.6 开发期间多次修改出厂预设的 systemPrompt（强化中文、加 thinking 指引）。
**问题**：用户已有 agent_presets.json，升级不会触发覆盖。
**解决**：`kPresetSchemaVersion` 整数（当前 3）；加载时若 `diskSchema < kPresetSchemaVersion`：
1. 收集所有 `readonly=false` 用户预设
2. `resetToDefaults()` 写入新版 5 个出厂预设
3. 把用户预设 upsert 回去
4. 写回 `schemaVersion = kPresetSchemaVersion`

**注意**：必须只覆盖 readonly，不能动用户的。bump 版本号需要谨慎，否则用户改过的 readonly 副本（其实复制后已是 readonly=false）不受影响。

### 问题 4.12：reasoning_content 回传策略
**M3 时观察**：DeepSeek `deepseek-reasoner` 早期版本 agent 第二轮报 HTTP 400 `"The reasoning_content in the thinking mode must be passed back to the API"`。
**M3 解决**：
- `ChatMessage` 加字段 `reasoningContent`
- SSE parser 把 `delta.reasoning_content` 拆通道，**与 content 分别累积**
- AgentLoop 写 assistant 消息时把 reasoningContent 一起塞回
- 序列化时 assistant 消息带 `reasoning_content` 字段

**M-1 (2026-05-24) 实测复核**（`tools/probe_reasoning.cpp`，deepseek-reasoner）：
- 组 A 回传 reasoning_content → HTTP 200 OK
- 组 B 剥离 reasoning_content → HTTP 200 OK
- 结论：服务端**已接受两种形式**。早期"必须回传"约束在某个时间点被放宽。
- 处理：保留回传逻辑（向下兼容、利于 UI 折叠面板复用），但纠正 `chat_provider.h:51-53` 与 `deepseek_chat_client.cpp:163-166` 注释口径。

### 问题 4.13：reasoning UI 与正文混在一起难读
**M3.3 旧做法**：把 `reasoning_content` 与 `content` 用 `[思考]` `[正文]` 文本前缀拼起来一锅塞 onDelta。
**问题**：长思考链占满气泡，正文淹没；用户没法只看结论。
**解决**：`onReasoningDelta` 独立通道 → ChatView 创建 `ReasoningBlock`（QToolButton 折叠按钮 + 灰色等宽 QLabel）。默认折叠；标题显字符数。完成后保留。
**对应**：M3 时代 K-09 "DeepSeek reasoner 思考链显示样式"已解决。

### 问题 4.14：反汇编右键 AI ▶ 子菜单 entryId 设计
**约束**：x64dbg `_plugin_menuaddentry` 一次注册的 entryId 在插件生命周期内有效，但预设可能动态增删，需要重建子菜单。
**问题**：直接 `entryId = baseId + presetIndex` 不稳定（删一个预设后所有 id 后移）。
**解决**：
- 静态 id 池 `2000–2099`
- 每次重建子菜单：`_plugin_menuclear(submenuId)` → 遍历当前 `showInContextMenu=true` 预设，从池里顺序取 id 分配
- `map<entryId, presetId>` 用 `std::mutex` 保护（菜单点击在调试线程，重建在主线程）
- handleMenuEntry 查 map 取 presetId → invokeMethod 到主线程跑 `runPresetById`

### 问题 4.15：恢复出厂"温和"语义
**早期实现**：恢复出厂 = `resetToDefaults()` 直接覆盖整个 presets 列表。
**问题**：用户已经辛苦定制的 readonly=false 预设全没了，差评。
**修正**：先收集所有 `readonly=false` 预设 → resetToDefaults → upsert 回去。只覆盖 5 个 readonly 出厂预设。

### 问题 4.16：enabledTools 全勾 vs 空集
**约束**：UI 上"全选"和"全空"两种边界状态语义模糊。
**规范**：
- `enabledTools` 为空集 = **使用全部工具**（约定）
- UI 上"全选"按钮触发后，自动归一为空集（避免列表长度爆炸 & 未来加新工具时旧预设自动获得）
- 用户主动只勾几个 → 存这几个的 id 列表
- AgentRunRequest 传给 ToolRegistry 时空集 → `listChatTools()` 全量

### 教训
- LLM 协议细节多（content=null / tool_call_id / tools 空数组陷阱），对协议错误日志要把完整请求 dump
- thinking 模型的 reasoning_content 协议契约语义**会变**：M-1 实测 deepseek-reasoner 已不再强制要求回传，对协议约束的"必须"陈述应留实证回归
- 把 loop 与 UI 严格分离（AgentLoop 不感知 Qt），后续可单测
- 用户配置项的 schema 一开始就要带 version，别等到改不动了再加

---

## M4 后：测试程序杂项修复

### 问题 D-01：reasoning_demo 终端中文乱码
**现象**：`reasoning_demo.exe` 在 cmd / Windows Terminal 默认 console 下，提示语显示为 `>>> T1: rsn::Mystery1(13) 鈥?璇蜂笅鏂悗鍥炶溅 (PRESS ENTER) <<<` 等 mojibake。
**根因**：`main.cpp` 是 UTF-8 无 BOM + CMakeLists 用 `/utf-8`（= `/source-charset:utf-8 /execution-charset:utf-8`），所以二进制里字符串字面量是 UTF-8 字节流；而 Windows console 默认 CP936 (GBK) 输出，按 GBK 解码 UTF-8 字节自然乱码。
**解决**：`main()` 开头加 `SetConsoleOutputCP(CP_UTF8)` 一行；双架构 rebuild。
**位置**：`tests/reasoning_demo/main.cpp:111`
**借鉴**：trace_demo / agent_demo 若后续遇到同类提示乱码，照搬即可。

### 问题 D-02：agent_demo 终端中文乱码（同 D-01）
**现象**：与 D-01 完全同根，`agent_demo.exe` cmd 下提示语 mojibake。
**解决**：`tests/agent_demo/main.cpp` 在 `main()` 入口加 `SetConsoleOutputCP(CP_UTF8)`；双架构 rebuild。
**位置**：`tests/agent_demo/main.cpp:81`

### 问题 D-03：fx_log1.txt 暴露的 agent 幻觉与"没代入入参"
**现象**：用户跑 reasoning_demo 时让"分析当前函数"预设分析 `Mystery1`，AI 给出的算法本质（`next_pow2`）正确，但有两处不达预期：
1. 用户期望"`Mystery1(13) = 16`"这种**带具体入参的数值演算**，AI 只给了 5/7/0x80000001 等通用样例，没代入用户实际跑的 13。
2. AI 主动推断"紧随其后的递归函数 Mystery2 内部调用了 Mystery1 作为辅助"——这是**幻觉**：Mystery2 用栈固定 `memo[64]` 数组，并没调 Mystery1。

**根因**：原 systemPrompt 只说"reason from concrete bytes/disasm, not guesses"，太弱。

**解决**：给 3 个出厂预设（`analyze-function` / `who-calls-here` / `string-api-context`）加两条强约束：
- **EVIDENCE RULE**：任何关于地址/函数体/调用关系/数据布局的断言必须基于本会话**实际做过的工具调用**结果；没读过的一律标"推测"或不说。禁止凭"看起来像"虚构调用边。
- **CONCRETE INPUT RULE**（仅 `analyze-function`）：当 user prompt 或初始 disasm 上下文里有具体入参值时，必须**代入逐步演算并报告结果**，不能只给通用算法描述。

**配套**：`kPresetSchemaVersion` 由 3 升到 4，触发 preset_store 升级路径：保留用户自定义 (readonly=false) 预设，仅覆盖 readonly 出厂预设（详见问题 4.11 修复套路）。

**位置**：`src/ai/agent_preset.cpp` 三处 `systemPrompt`；`src/ai/agent_preset.h:30` 版本号 bump。

### 问题 D-04：技术债批量收尾（K-04 / K-05 / K-13）
本轮顺手做掉 3 个低成本技术债：
- **K-04**：`assistant_panel.cpp` 头文件块按字母序补 `#include <QStyle>`（之前靠间接 include，换 Qt 版本可能崩）
- **K-05**：`TraceDialog::rebuildStackTree()` 末尾追加 `stackTree_->expandToDepth(2)`，CallStack 模式采样完成默认展开两层
- **K-13**：`AssistantPanel` 加 `agentTerminalEventHandled_` 标志，`failed` / `maxIterReached` 路径置 true；`finished` 处理时若已置 true 跳过 `XAI_LOG_INFO("agent finished ...")` 冗余日志，但仍保险地调一次 `setAgentRunning(false)`（幂等）。
- 三项都已在双架构 Release 重编通过；详见 `known-issues.md`。

---

## S0：自动化调试紧急修复（2026-05-24）

> 触发：`docs/review-auto-debug.md` 给出 2 Critical + 1 High。本轮一次清完，建立 baseline tag `s0-baseline` → 推进到 `s0-done`。

### 问题 S0-C1：CB_STOPDEBUG 双注册导致 ProjectContext.onDebugStop 丢失
x64dbg SDK 的 `_plugin_registercallback` 对同 `(plugin, type)` 后注册者覆盖前注册者。`plugin_callbacks` 与 `trace_recorder` 都注册了 `CB_STOPDEBUG`，且 trace 在后，导致 `AssistantPanel::onDebugStopped` / `ProjectContext::onDebugStop` 永不触发。

修复：
- `src/plugin/plugin_callbacks.cpp::cbStopDebug` 改为单点分发，依次调用
  - `AssistantPanel::onDebugStopped`
  - `TraceRecorder::onStopDebug`
  - `CallStackTracer::onStopDebug`
  - `ProjectContext::onDebugStop`
- `src/trace/trace_recorder.cpp` 移除 `CB_STOPDEBUG` 的 register/unregister，删去内部静态 `cbStopDebug` 函数。
- `TraceRecorder::onStopDebug` / `CallStackTracer::onStopDebug` 原本已是 public 方法，零额外改动。

### 问题 S0-C2：cbInitDebug 主线程同步 SHA256 + sqlite 阻塞 UI
几百 MB 的 exe 在调试启动时，调试器主线程同步算 SHA256 → 打开 sqlite → 建表 / 写 meta，UI 卡 2–6 秒。

修复（`src/storage/project_context.{h,cpp}`）：
- `onDebugStart` 主线程部分：抢占 `generation_++`，清空 `store_` 与 `projectId_`，记录 `mainModulePath_`，置 `indexing_ = true`，立即返回。
- 后台 detach 线程：算 SHA256 → 构造 `SessionStore` → 写 meta keys；落盘前后两次比较 `generation_`，若被新一轮 `onDebugStart` 或 `onDebugStop` 抢占就丢弃结果。
- 新增 `isIndexing()` 供 UI 灰化 Agent 入口。
- 所有消费者（24 处 `ProjectContext::instance().store()`）原本就 nullptr 软返回，零外部修改。

`onDebugStop` 同样自增 `generation_`，使在途的 SHA256 线程到达落盘点时静默丢弃，避免"调试已停 store 又被装回"的回填竞态。

### 问题 S0-H1：read_memory.size / get_disasm.lines 拒绝字符串化数字
`is_number_integer()` 严格校验导致 LLM 传 `"size": "256"` 或 `"size": "0x100"` 时反复失败到 maxIter。

修复（`src/ai/tools/basic_read_tools.cpp`）：
- 新增 `parseInt32Lenient(json, lo, hi, out, err)`：接受 JSON number（含 float 截断）/ 十进制字符串 / `0x...` 十六进制字符串；clamp 到 `[lo, hi]`；错误消息显式包含范围。
- `read_memory.size` 改用 `parseInt32Lenient(v, 1, 65536, ...)`。
- `get_disasm.lines` 改用 `parseInt32Lenient(v, 1, 512, ...)`，原本就有 `std::clamp` 但不接受字符串，现统一。
- `dynamic_context_tools.cpp` / `static_analysis_tools.cpp` 中 `max_frames` / `limit` / `top_k` / `max_results` 等可选 hint 参数走默认值兜底（字符串只导致回退到 default，不会让调用失败），属体验改进，列为 S1 顺手处理。

### M-1 复核确认（已写入审查报告与 features/architecture）
独立编出 `tools/probe_reasoning.exe`（不复用 `src/`，只 link cpr + nlohmann + Crypt32 + DPAPI），三轮请求 deepseek-reasoner：
- ROUND1 普通请求：HTTP 200，content=3 字、reasoning=76 字。
- GROUP_A 多轮回传 reasoning：HTTP 200。
- GROUP_B 多轮剥离 reasoning：HTTP 200。

结论：早期"必须回传 reasoning_content"的约束已被官方放宽。保留现有"回传"逻辑（兼容协议未来收紧 + 复用 UI 折叠面板），仅修正注释口径（`chat_provider.h:51-57`、`deepseek_chat_client.cpp:163-167`）。

### 构建与验证
- x64 / x86 双架构 Release 编通过：`build-x64/bin/Release/x64dbg_ai_plugin.dp64`、`build-x86/bin/Release/x64dbg_ai_plugin.dp32`。
- Git baseline commit `2452f9b` + tag `s0-baseline`；S0 完成后另打 tag `s0-done`。

---

## S1：纯读工具补全（2026-05-24）

> 触发：审查报告 §5 P2 列出 T-11..T-14；核对源码后 T-13 `get_registers`（basic_read_tools.cpp:378）与 T-14 `get_callstack`（dynamic_context_tools.cpp:77）已存在且实现合理，S1 实际只需补 T-11 / T-12。

### T-11 `eval_expression`
- 入口：`DbgEval(expr, &ok)`（`bridgemain.h:1239`）。
- 入参：`expr`（必填 string，1–512 字符）。
- 返回：`{expr, value: "0x...", value_dec: "..."}`。
- 失败语义：`DbgEval` 内部失败 → `r.ok=false, r.error="expression evaluation failed: <expr>"`，不抛异常。
- 用例：LLM 让"读 [rbp+8] 的指针所指的字符串"先 `eval_expression("[rbp+8]")` 拿到地址再 `read_string`。
- 位置：`src/ai/tools/basic_read_tools.cpp` EvalExpressionTool。

### T-12 `list_breakpoints`
- 入口：`DbgGetBpList(BPXTYPE, BPMAP*)`（`bridgemain.h:1162`）+ `BridgeFree`。
- 入参：`type`（可选 string："all"|"software"|"hardware"|"memory"|"dll"|"exception"，默认 all）。
- 返回字段：`{count, truncated, breakpoints:[{type, addr, enabled, active, singleshoot, hitCount, mod, name}]}`。
- 硬上限：1024 条，超出 `truncated=true`，防止 trace 断点把 LLM 灌爆。
- 位置：`src/ai/tools/basic_read_tools.cpp` ListBreakpointsTool。
- maxResultBytes：64 KB（与 list_modules 持平）。

### 预设白名单 + schema v5
- `analyze-function` / `who-calls-here` 两个最相关的预设白名单追加 `eval_expression` / `list_breakpoints`。
- "解释此处"（explain-here）保持精简不变。
- `agent_preset.h::kPresetSchemaVersion` 4 → 5，让现存用户的旧 preset.json 在下次启动触发"覆盖 readonly=true 项、保留用户自定义"逻辑，无缝吃到新工具。

### 测试
- x64 / x86 双架构 Release 编通过。
- Agent 实测留作 S0+S1 端到端验证（参见后续"S1 烟测"小节，目前未执行）。
- Git tag：`s1-done`。

> 关于 P2 体验改进 K-17 残余项：`dynamic_context_tools.cpp` 的 `max_frames`、`limit`、`top_k`，以及 `static_analysis_tools.cpp` 的 `max_results`，目前 `is_number_integer()` 严格但默认值兜底（字符串只是不生效），不构成 bug，留 S2 顺手统一为 `parseInt32Lenient`。

---

## S2：调试器事件总线 + wait_for_event（2026-05-24）

> 触发：审查报告 §5 P0 T-06；现状 cbBreakpoint 单点注册导致 trace_recorder/callstack_tracer 抢占；agent 无法在 step/continue 后阻塞等下一次中断，只能盲目重试。

### S2-A `dbg/event_bus.{h,cpp}`
- 新建 `EventBus` 单例。`enum class DbgEvent { Breakpoint, Paused, Resumed, Stepped, DebugStarted, DebugStopped }`。
- API：`subscribe(ev, handler)→Token` / `unsubscribe(Token)` / `waitOnce(ev, timeout, &payload, cancelFlag)` / `publish(ev, payload)` / `cancelAllWaits()`。
- 关键设计：
  - `publish`：把 handler 列表拷贝到栈上再释放锁触发，允许 handler 内部 sub/unsubscribe 不死锁。
  - `waitOnce`：condition_variable + 50ms 切片轮询 `cancelFlag`，粒度足够 agent 用且能秒级响应取消。
  - handler 同步在调试线程跑，约束"短小不嵌套 SDK 命令"（写在头文件注释里）。

### S2-B `plugin/plugin_callbacks.cpp`
- 统一注册 7 个 SDK 回调（INITDEBUG/STOPDEBUG/MENUENTRY/BREAKPOINT/PAUSEDEBUG/RESUMEDEBUG/STEPPED），4 类调试事件通过 `EventBus::publish` 单源对外。
- `cbInitDebug`/`cbStopDebug` 同步发 DebugStarted/DebugStopped，并在 stop 时 `cancelAllWaits` 防止 agent 工具线程一直卡到 timeout。
- 新增 `unregisterCallbacks` 对称卸载。

### S2-C 老消费者迁移
- `trace_recorder.cpp` 删除 cbBreakpoint 自注册，改 `EventBus::subscribe(Breakpoint, ...)`；`trace_recorder.h` 加 `bpToken_{0}` 字段记录订阅。
- `callstack_tracer.cpp` 没有 registerCallbacks（之前被 trace_recorder 顺手叫），现在由 plugin_callbacks 直接 `EventBus::subscribe(Breakpoint, [](p){ CallStackTracer::instance().onBreakpoint(p.addr); })`，消除"两个消费者要竞争同一个 CB_BREAKPOINT 回调，后注册者覆盖前者"的隐患。

### S2-D `wait_for_event` 工具 + ToolContext.cancelFlag
- 新建 `src/ai/tools/debug_control_tools.cpp`，`wait_for_event` 接受 `event ∈ {breakpoint, paused, stepped, any}` + `timeout_ms ∈ [100, 60000]`，默认 5000ms。
- "any" 实现：三路 50ms 切片轮询 Breakpoint/Paused/Stepped。
- 失败分流：`reason ∈ {timeout, cancelled, debug_stopped}`，agent 能根据原因决定继续等还是放弃。
- 工具运行在 AgentWorker 的 `QtConcurrent::run` 后台线程里，调用 `Script::Debug::Wait` 类的阻塞 API 不会卡 GUI。
- **顺带修一个隐性缺陷**：`ToolContext` 加 `const std::atomic<bool>* cancelFlag`；`AgentWorker` 把它指向 worker 的 `cancel` 标志。这样所有"工具内部有循环等待"的实现都能 1s 内响应用户取消，不再被超时压垮（实际 wait_for_event 最大 60s）。

### S2-E `parseInt32Lenient` 提到 `tool_args_util.h`
- 新建头文件 `src/ai/tools/tool_args_util.h`，inline 暴露 `parseInt32Lenient` + `tryGetInt32Hint`（已存在性 + 可解析性双检查）。
- 删除 `basic_read_tools.cpp` 内的私有副本，改 `#include`；`dynamic_context_tools.cpp` 把 max_frames / limit / max_results / top_k 4 个 hint 全部换成 `tryGetInt32Hint`（同时返回友好错误而非默默使用默认值）；`static_analysis_tools.cpp` 两个 max_results 同步迁移。
- 解决 K-17 残余：LLM 把 "32" / "0x20" 当字符串传过来不再被悄悄忽略默认 64，agent 减少一轮无效重试。

### S2-F 预设升级
- `kPresetSchemaVersion` 5 → 6，触发 readonly preset 覆盖逻辑，让现存用户启动即吃到 wait_for_event。
- 通用预设 `analyze-function` 白名单追加 `wait_for_event`；其他 3 个静态分析预设刻意不开（agent 在纯静态场景不应 hang）。

### S2-G 构建 / 文档 / tag
- `src/CMakeLists.txt` PLUGIN_SOURCES 加 `dbg/event_bus.{h,cpp}` / `ai/tools/tool_args_util.h` / `ai/tools/debug_control_tools.cpp`。
- 双架构 Release 编译通过（`build-x64\bin\Release\x64dbg_ai_plugin.dp64` / `build-x86\...\dp32`）。
- Agent 实测留 S3 之前的端到端验证。
- Git tag：`s2-done`。

### 副作用与下一步
- `EventBus::waitOnce` 的 `cancelFlag` 参数类型 `std::atomic<bool>*` → `const std::atomic<bool>*`（只 load，不改），匹配 ToolContext 的 const 指针；已同步头/实现两端。
- S3 起进入 ToolPolicy + 写工具五件套（T-01..T-05），write_audit.log，5s 倒计时 confirm，run_dbg_command 白名单。

---

## S3：写工具五件套 + ToolPolicy + 5s confirm + write_audit.log（2026-05-24）

> 目标：让 agent 真正能"驱动调试器"——下断点 / 单步 / 跑到指定地址 / 通用命令，但每次写操作都过用户 5s 倒计时确认，并落独立审计日志。

### S3-A `ai/tools/tool_policy.h`
- 新建 `enum class ToolCategory { Read, DbgControl, Write }`。
- `ITool::category()` 加默认实现 `return ToolCategory::Read`，子类按需覆盖。
- 配套 `toolCategoryName()` 让 audit 日志可读。

### S3-B `util/logging.{h,cpp}`
- 加 `auditLog()` 全局函数；初始化时建独立 spdlog logger `x64dbg-ai-audit`，sink 单独的 `write_audit.log`（4 MB × 10 轮转），pattern `%v`（无任何装饰，每条直接是完整 JSON）。
- `flush_on(info)` 保证写工具行为即时落盘，进程崩了也不丢。
- `shutdownLogging()` 同步关闭。

### S3-C `ui/tool_confirm_dialog.{h,cpp}`
- `ToolConfirmDialog`：模态 QDialog，5 秒倒计时；"允许"按钮初始 disabled，文本 `允许 (Ns)`，QTimer 每秒 -1，归零后启用并去掉计数。
- `denyButton_->setDefault(true)` → ESC/Enter 默认拒绝；安全为先。
- 静态 `confirmFromBackground(toolName, summary, argsJson, sec)`：工具线程入口。`QThread::currentThread() != app->thread()` 时用 `QMetaObject::invokeMethod(app, lambda, BlockingQueuedConnection)` 切到 GUI 线程并阻塞等返回。无 `QApplication` 时安全 deny。
- 参数 JSON 在等宽字体 dark 主题块内展示，高级用户可审。

### S3-D `ToolRegistry::dispatch` 接入 policy
- 新增 `ToolContext::confirmCallback`（`std::function<bool(name,summary,argsPretty)>`），AgentWorker 在 ctx 构造时塞入 `ToolConfirmDialog::confirmFromBackground` 的 lambda。
- dispatch 在 invoke 前判 `category()`：
  - **Read**：原路通过。
  - **DbgControl**：写 audit `phase=begin` → invoke → 写 `phase=end {ok, error, data_snippet, elapsed_ms}`。不弹 confirm（agent 主动等下次中断是合理行为，弹窗反而打断）。
  - **Write**：调 `confirmCallback`。无 callback → 直接 deny + 写 `phase=denied_no_ui`。confirmed → 写 `phase=confirmed` 再 invoke 再写 end；denied → 写 `phase=denied_by_user`。
- JSON 字段：`ts(ms epoch) / tool / category / args / phase / sha / session [/ ok / error / data_snippet / elapsed_ms]`，一行一条便于 grep/jq。

### S3-E/F/G `debug_write_tools.cpp` 五件套 + 命令逃生口
- `set_breakpoint(addr, type=software|hardware)` → `Script::Debug::SetBreakpoint` / `SetHardwareBreakpoint`。
- `remove_breakpoint(addr)` → 软硬都试一遍，返回 `{software_removed, hardware_removed}`。
- `step_in(timeout_ms=30000)` → `DbgCmdExecDirect("StepInto")` + `waitForStop`（私有 helper：50ms 切片三路轮询 Paused/Breakpoint/Stepped，响应 cancelFlag 和 `!DbgIsDebugging()`）。
- `step_over(timeout_ms=30000)` → `StepOver` + waitForStop。
- `run_until(addr, timeout_ms=30000)` → `DbgCmdExecDirect("bp 0x.., ss")` 装 one-shot + `run` + waitForStop + 兜底 `DeleteBreakpoint`（singleshoot 命中后 x64dbg 也会清，但 timeout 路径必须自己清）。
- `run_dbg_command(command)` → 首 token（按空白 / 逗号切）转小写后查 15 token 白名单：
  - 断点：`bp/bpc/bphwc/bpd/bpe`
  - 控制：`run/stepinto/stepover/stepout/pause`
  - 数据读：`db/dw/dd/dq`
  非白名单直接 deny + warn。允许后 `DbgCmdExecDirect` 同步执行；返回 `{command, executed}`。
- 所有工具 `category()==Write` + `requiresUserConfirmation()==true`。`wait_for_event` 提级为 `DbgControl`（不要 confirm 但要 audit）。

### S3-H 预设升级
- `kPresetSchemaVersion` 6 → 7，触发现有 readonly preset 自动覆盖。
- 通用预设 `analyze-function` 白名单追加 6 个新工具；其他 3 个静态分析预设保持精简，不开写工具。

### S3-I 构建 / 文档 / tag
- `src/CMakeLists.txt` 加 `ai/tools/tool_policy.h` / `ai/tools/debug_write_tools.cpp` / `ui/tool_confirm_dialog.{h,cpp}`。
- 双架构 Release 编译通过，零警告（dp64 / dp32 产物已就位）。
- Git tag：`s3-done`。

### 设计取舍记录
- **autoApprove 字段没做**：preset.json 加白名单跳 confirm 风险大，等用户反馈再评估。当前所有写工具一律 5s 弹窗，不可绕过。
- **run_until 的 one-shot 走命令而非 Script API**：因为 `Script::Debug::SetBreakpoint` 不支持 singleshoot 标志；DbgCmdExecDirect 同步发 `bp addr, ss` 是公开且稳定的实现路径。
- **wait_for_event 不弹 confirm**：它本质是只读（agent 等下次中断信号），但占用执行流且涉及 cancelFlag 通信，需要 audit 追踪 agent 是否合理使用（不要被 LLM 当 polling 用）。
- **审计 sink 单独 logger 而非给 plugin.log 加 tag**：JSON 一行一条要求 pattern 是裸 `%v`，与 plugin.log 的 `[ts] [tid] [lvl] %v` 冲突；分离最清爽。

---

## S4：数据写工具三件套 patch_memory / set_register / write_string（2026-05-24）

> 目标：补齐内存补丁、寄存器修改、字符串写入；让 agent 真正能改运行时状态。沿用 S3 的 ToolPolicy/confirm/audit 闭环，无新基础设施。

### S4-A patch_memory
- 入参：`addr` + `bytes_hex`；hex 解析支持 `DE AD BE EF` / `deadbeef` / `DE,AD,BE,EF` / `de:ad:be:ef` / `de-ad` 多种分隔；不允许通配符（精确写入）。
- 越界检查：4 KB 步进 + 末字节 `DbgMemIsValidReadPtr`，避免跨页 partial write。
- 写入上限：4 KB（防 prompt 注入超大写）。
- `DbgMemWrite` 直调；失败时报具体 VA 和 size。
- audit data_snippet 含 `bytes_pretty="DE AD BE EF...(+N)"` 便于事后核对。

### S4-B set_register
- 新建寄存器名表 `regTable()`：90+ 条，含 GPR / 子寄存器（AL/AH/AX/EAX/RAX 等，含 R8B/R9W/SIL/SPL/BPL/DIL）/ DR0-DR7 / EFLAGS / 架构无关别名 Cxx。x86 / x64 用 `#ifdef _WIN64` 条件编译。
- 不支持：XMM/YMM/MXCSR/FPU（`Script::Register::Set` 没暴露入口）。
- byteWidth 校验：写 AL 传 0x100 直接拒，避免静默 truncate。
- name 大小写不敏感（`rax` / `RAX` / `Rax` 等价）。

### S4-C write_string
- 三种编码：`utf8`（默认）/ `utf16le` / `ascii`。
- ASCII 路径拒绝 >0x7F 字节，避免静默 mojibake（用户传中文必须显式 utf8/utf16le）。
- UTF-16LE 路径用 `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` 先解码再按 2 字节 LE 输出；非法 UTF-8 直接报错。
- 自动追加正确长度的 `\0` 终止（1 byte / 2 byte）。
- 编码后硬上限 8 KB；源串上限 4 KB。
- audit data_snippet 含前 80 字符 `value_preview` 便于复盘。

### S4-D/E 集成
- `ai/tools/data_write_tools.cpp` 新建；`builtin_tools.h` 暴露 `registerDataWriteTools`；`tool_registry.cpp::registerBuiltinTools` 注册。
- `src/CMakeLists.txt` PLUGIN_SOURCES 追加。
- `kPresetSchemaVersion` 7 → 8；`analyze-function` 白名单追加三件套。其他 3 个静态分析预设保持精简，不开数据写。

### S4-F 构建
- 双架构 Release 编译通过，零警告。dp64/dp32 已就位。
- Git tag：`s4-done`。

### 设计取舍
- **patch_memory 与 write_string 都用 `DbgMemWrite`**：底层同一 API；write_string 只是预处理编码 + 自动 \0。理论上可让 agent 用 patch_memory 自己组字节，但 write_string 让 `encoding` 显式化，audit 日志可读性更强（`encoding=utf16le` 一眼看出意图）。
- **set_register 不做 EFLAGS 位运算辅助**：传完整的 32/64 位值；位操作让 agent 用 `eval_expression` 算好再传。简化白名单维护。
- **XMM/AVX 不做**：x64dbg Script API 没暴露 setter；后期如需要必须走 `DbgValToString`/`DbgValFromString` + 命令字符串，复杂度高。


## S5：脚本工具三件套 list_scripts / load_script / run_script_file（2026-05-24）

> 目标：把 x64dbg 的 Script 子系统暴露给 agent，让 LLM 能挂载用户预写的脚本做批量自动化（解密循环、批量补丁、自动化探测等）。
> 沿用 S3 的 ToolPolicy + 5s confirm + 双相 audit 闭环；脚本目录约定在 `%APPDATA%\x64dbg-ai-plugin\scripts\`。

### 工具
- `list_scripts()` — category=Read。枚举 scripts/ 下 *.txt / *.script，返回 name / size_bytes / mtime_ms。
- `load_script(path)` — category=Write + confirm。`DbgScriptUnload` + `DbgScriptLoad`，加载但不执行。
- `run_script_file(path)` — category=Write + confirm。Load + `DbgScriptRun(0)`，fire-and-forget。

### 路径与限制
- 相对路径 → 接 `pluginScriptsDir()`；绝对路径直接用（confirm 弹窗能让用户看到完整路径）。
- 仅接受 `.txt` / `.script` 扩展，1 MB 上限。
- 文件 mtime 转 UNIX ms：用 Win32 `GetFileAttributesExW` + FILETIME→epoch 偏移（116444736000000000 个 100ns），绕开 MSVC `file_clock::to_sys` / `clock_cast` 兼容坑。

### 设计取舍
- **run_script_file 不能同步等待**：x64dbg SDK 的 `DbgScriptRun` 异步且无完成事件，所以工具立刻 return started=true。description 教 LLM 用 `wait_for_event(Paused/Breakpoint)` 或后续 `get_registers/read_memory` 观察副作用。已记 K-22。
- **load_script 单独存在**：让 agent 可以"先加载、人工预览、再 Run"，对高风险脚本多一道保险。
- **list_scripts 只列文件名 + meta，不读内容**：内容预览交给现有 host 文件系统能力（或后续可加 read_script_text，但本阶段不做）。
- **未引入 save_script**：目前没有 agent 写脚本的需求；如需要可后续加 W-4，做 sandbox + size cap。

### S5-A 集成
- `ai/tools/script_tools.cpp` 新建；`util/paths.{h,cpp}` 加 `pluginScriptsDir()`。
- `builtin_tools.h` 暴露 `registerScriptTools`；`tool_registry.cpp::registerBuiltinTools` 注册。
- `src/CMakeLists.txt` PLUGIN_SOURCES 追加。
- `kPresetSchemaVersion` 8 → 9；`analyze-function` 白名单追加三件套（其他静态分析预设不开脚本）。

### S5-B 构建
- 双架构 Release 编译通过，零警告。dp64 / dp32 已就位。
- 工具总数：14 读 + 1 控制 + 9 写（S3+S4）+ 3 脚本（S5，含 1 读 + 2 写）= **27 工具**（15 读 / 1 控制 / 11 写）。
- Git tag：`s5-done`。


## S6：基础控制 + 沉淀（label/comment）+ 程序地图（2026-05-24）

> 目标：补齐 agent 真正需要却之前缺位的"基础控制"（继续运行/异步暂停/跳出函数），让 LLM 把分析结论"沉淀"为 x64dbg 原生的 label/comment（跨会话持久化），并把"程序结构"作为高层次工具暴露（函数表/内存映射/IAT/EAT）。沿用 S3 的 ToolPolicy + 5s confirm + 双相 audit。

### 工具分组（新增 10 个；总 27 → 37）

#### S6-A 调试导航（`ai/tools/debug_navigation_tools.cpp`）
- `run_continue(wait_for_stop=false, timeout_ms=30000)` — Write+confirm。`Script::Debug::Run()`；默认不阻塞 agent loop（让 LLM 能立刻安排下一步策略）；`wait_for_stop=true` 时调用 `Script::Debug::Wait` 等到 Paused/超时。
- `pause_debug(timeout_ms=5000)` — Write+confirm。`Script::Debug::Pause()` 后等 Paused，5s 兜底。
- `step_out(timeout_ms=30000)` — Write+confirm。`Script::Debug::StepOut()` + waitForStop。

#### S6-B/C 沉淀（`ai/tools/annotation_tools.cpp`）
- `set_label(address, text)` / `set_comment(address, text)` — Write+confirm。**`text=""` 即删除**（不另开 delete_* 工具，减少同义工具对 LLM 决策的噪声）。UTF-8 ≤255 字节。底层是 `Script::Label::Set/Delete` 与 `Script::Comment::Set/Delete`，写入 `.dd64` 数据库，跨会话持久化。
- `get_label(address)` / `get_comment(address)` — Read。单 VA 查询。
- `list_labels()` / `list_comments()` — Read。用 `BridgeList<T>` RAII 取 `ListInfo`，免手写 BridgeFree。硬截断 256 KB。

#### S6-D 内存映射（`ai/tools/program_map_tools.cpp` 上半）
- `get_memory_map()` — Read。`DbgMemMap` 遍历 `MEMPAGE`；每页返回 base/size/state(commit/reserve/free)/type(image/mapped/private)/protect（**RWX 风格人类可读字符串**，如 "RWX"/"R-X"/"---+G"）/info（模块或段名）。protect 不直接抛 PAGE_* 数值给 LLM，便于推理（识别可疑 RWX 私有页等）。返回完毕后 `BridgeFree(mm.page)`，遵守 SDK 约定。
- `get_page_protect(address)` — Read。`Script::Memory::GetProtect` + GetBase/GetSize；返回 protect 字符串 + 原始 DWORD + 所在页基址/大小。
- `set_page_protect(address, protect, size)` — Write+confirm。protect 接受 "RW"/"RWX"/"R-X"/"---" 等 RWX 风格字符串；本工具**不支持** PAGE_GUARD/PAGE_NOCACHE/PAGE_WRITECOMBINE 等修饰位（避免 LLM 误用）；size 上限 16 MB。

#### S6-E 程序地图（`ai/tools/program_map_tools.cpp` 下半）
- `list_functions(module?)` — Read。`Script::Function::GetList`；`module` 是大小写不敏感子串过滤。每条返回 module / rva_start / rva_end / manual / instruction_count。硬截断 256 KB（大程序的全量函数表很大）。
- `get_module_imports(module)` — Read。`Script::Module::GetImports`，逐条返回 name / undecorated / ordinal / iat_va / iat_rva。LLM 可按 API 类别聚类（crypto / net / file / anti-debug）做 triage。
- `get_module_exports(module)` — Read。`Script::Module::GetExports`，多了 forwarded(+forward_name) 字段。

### 预设（新增 2 个；总 5 → 7）
- `annotate-function`：白名单 13 个工具（11 读 + `set_label` + `set_comment`），systemPrompt 强制工作流"分析 → 命名（snake_case ASCII label） → 写注释（中文 ≤80 字）"；先 `list_labels` 避免覆盖更权威的人工标注。
- `map-program`：白名单 8 个纯读工具（list_modules + memory_map + page_protect + list_functions + imports + exports + list_labels + rag_search）；专门给"新样本初探"用，绝不开任何写工具。

`analyze-function` 同步扩白名单：把 S6 新增的 10 个工具全加进去（保持它作为"全能预设"的定位），总数 = 全 37 工具。

### 设计取舍
- **`text=""` 表示删除**：早期方案是另开 `delete_label`/`delete_comment`，但同义工具会让 LLM 在"是该 set 空串还是 delete"上犹豫。取消独立 delete 工具后白名单更紧。
- **run_continue 默认不阻塞**：x64dbg 跑起来可能持续很久，阻塞 agent loop 没意义。LLM 想等 Paused 时显式 `wait_for_event` 或下次工具调用前用 `wait_for_stop=true`。
- **protect 字符串而非 DWORD**：LLM 对 "RWX" 比 "0x40" 更敏感，几乎不会写错；代价是 set_page_protect 不支持修饰位（暂未发现 agent 场景需要）。
- **list_functions/labels/comments 截断 256 KB**：远大于默认 64 KB；这些工具天生整批返回，截断后给 LLM 加 module 过滤就够用。
- **set_page_protect 是 Write+confirm**：改 RWX 可让 IAT/CFG 完整性被破坏，必须 5s 倒计时让用户能取消。

### S6-F 集成
- `ai/tools/debug_navigation_tools.cpp` / `annotation_tools.cpp` / `program_map_tools.cpp` 新建，分别 `registerDebugNavigationTools` / `registerAnnotationTools` / `registerProgramMapTools`。
- `builtin_tools.h` 加三个 register 声明；`tool_registry.cpp::registerBuiltinTools` 末尾加三个调用。
- `src/CMakeLists.txt` PLUGIN_SOURCES 追加三个 .cpp。
- `kPresetSchemaVersion` 9 → 10；`agent_preset.cpp` 加 `annotate-function` / `map-program` 两预设；`analyze-function` 白名单扩 10。

### S6-G 编译坑
- `Script::Label::Set` 有 3 参（默认 manual=false）和 4 参（带 temporary）两个重载；只传 3 个时编译报 C2668（"对重载函数的调用不明确"）。修复：显式传第 4 个 `/*temporary=*/false`。`Script::Comment::Set` 只有一个三参重载，无此问题。
- 文件 mtime 沿用 S5 的 `GetFileAttributesExW` + FILETIME→epoch 偏移方案（虽然 S6 用不到，记一下以防 S7 复用）。

### S6-H 构建
- 双架构 Release 编译通过，零警告零错误。dp64 / dp32 已就位。
- 工具总数：26 读 + 3 控制 + 8 写 = **37 工具**。
- 预设总数：7（含 freeform / analyze-function / who-calls-here / string-api-context / explain-here / **annotate-function** / **map-program**）。
- Git tag：`s6-done`。


## S7：高级断点 + 汇编 + CFG + 补丁 + GUI 焦点（2026-05-24）

> 目标：把 agent 从"分析者"升级为"操作者"。补齐硬件断点 / 条件断点 / 汇编写入 / 模式批量替换 / CFG 可视化 / 标志位翻转 / 补丁审计与回滚 / x64dbg 原生模板渲染 / GUI 焦点引导，共 12 个新工具，全面覆盖破解 / 反反调试 / 数据流追踪三类高价值场景。

### 工具分组（新增 12 个；总 37 → 49；29 读 + 5 控制 + 15 写）

#### S7-A 高级断点（`ai/tools/advanced_bp_tools.cpp` 上半）
- `set_hw_breakpoint(address, type=execute|write|access)` — Write+confirm。`Script::Debug::SetHardwareBreakpoint(addr, HardwareType)`；**注意 SDK 不暴露 size 参数**（DR7 LEN 由 type 决定，全部按 1 字节），如需指定 size 必须手写 `bphws` 命令。DR0–DR3 共 4 槽，超限 SDK 直接返 false，工具如实报错"possible cause: 4 HW BP slots exhausted"让 LLM 自我修正。
- `remove_hw_breakpoint(address)` — Write+confirm。`Script::Debug::DeleteHardwareBreakpoint`；不存在不报错（read-after-write 语义），返回 `removed=false` 让 LLM 知道实际状态。

#### S7-B 条件断点（`ai/tools/advanced_bp_tools.cpp` 下半）
- `set_conditional_bp(address, condition?, logText?, logCondition?, command?, commandCondition?, fastResume?, silent?, name?)` — Write+confirm。**这是 S7 唯一一个"修改已有断点字段"的工具**，调用前用 `BpRefVa(&ref, bp_normal, va)` 把 VA 解成 `BP_REF`（_dbgfunctions.h:169-191），再用 `BpSetFieldText/Number(&ref, bpf_*, value)` 改字段。空串清字段。**任意字段中途失败立即返**（与 question 确认的"不回滚"策略一致），返回 `applied` 字段告诉 LLM 哪些已生效以便修复。前置条件：必须先 `set_breakpoint` 创建软断点，否则 `BpRefVa` 返 false。

#### S7-C 汇编（`ai/tools/assembler_pattern_tools.cpp` 上半）
- `assemble_at(address, instruction, fill_nop=true)` — Write+confirm。`Script::Assembler::AssembleMemEx(addr, asm, &size, errbuf, fillnop)`；**默认 fill_nop=true**（与 GUI 行为一致；用户确认的选项），新指令短于原指令时自动 NOP 填充。错误缓冲 `MAX_ERROR_SIZE=512`（bridgemain.h:196），失败时把 capstone 报错原文带回，避免 LLM 瞎猜（典型："invalid instruction" / "invalid register"）。

#### S7-D 模式替换（`ai/tools/assembler_pattern_tools.cpp` 中段）
- `pattern_replace(start, size, search_pattern, replace_pattern)` — Write+confirm。`Script::Pattern::SearchAndReplaceMem(start, size, search, replace)`，**`??` 通配** SDK 已内置，工具层不做掩码计算。size≤16MB（与 set_page_protect 同安全线，防止 LLM 写出 `size=进程总虚拟空间` 类悲剧调用）。

#### S7-E CFG（`ai/tools/cfg_tool.cpp`）
- `get_cfg(entry)` — Read。最复杂的一个工具。`DbgAnalyzeFunction(entry, &BridgeCFGraphList)`（bridgemain.h:1238）→ 用 `BridgeCFGraph(list, /*freedata=*/true)` C++ wrapper（bridgegraph.h）RAII 接管节点数组（避免手写 `BridgeFree(data)` 漏掉），自动转 `std::unordered_map<duint, BridgeCFNode>` + `parents` 反向边。输出按用户确认的方案：**单一 Mermaid `graph TD`**：
  - 节点：`N_<hex>["<start>..<end>\n<icount> insn[ RET][ ICALL][ SPLIT]"]`
  - 边：无条件 `-->`；条件 `-->|T|` (brtrue) / `-->|F|` (brfalse)
  - 入口节点 `classDef entry fill:#fcc,stroke:#900` 高亮
  - 节点数 > 256 时截断（mermaid.js 前端默认 maxEdges=500，留余量）
  - 返回 `{entry, nodes, truncated, mermaid}`

#### S7-F 标志位（`ai/tools/assembler_pattern_tools.cpp` 下半）
- `set_flag(name, value)` — Write+confirm。name → `Script::Flag::FlagEnum` 9 选 1（ZF/OF/CF/PF/SF/TF/AF/DF/IF）。这是"快速试验某分支走法"的低成本写工具：先 set_flag 跑一遍看路径，再决定要不要 assemble_at 做持久补丁。

#### S7-G 补丁审计（`ai/tools/patch_misc_tools.cpp` 上半）
- `list_patches(module?)` — Read。`DbgFunctions()->PatchEnum` 两阶段查询：第一次 `PatchEnum(nullptr, &cbsize)` 拿大小，第二次填 `vector<DBGPATCHINFO>`。模块名子串大小写不敏感过滤；硬上限 4096 条；**每条 = 单字节 diff**（N 字节补丁出现 N 条记录），让 LLM 知道总规模。
- `restore_patch(address)` — Write+confirm。`DbgFunctions()->PatchRestore`。无补丁时 SDK 返 false，工具如实报"no patch at <VA>?"。

#### S7-H 模板（`ai/tools/patch_misc_tools.cpp` 中段）
- `format_with_dbg(template)` — Read。`DbgFunctions()->StringFormatInline(fmt, size, out)`（_dbgfunctions.h:239）。支持 x64dbg 原生表达式 `{rax}` / `{x:[rsp+8]}` / `{s:[rcx]}`。预分配 4 KB `vector<char>` 缓冲；失败（模板不合法）报错。这是 trace-input 预设的关键工具——在每次断点命中时按模板渲染 `[rcx] = "{s:[rcx]}"` 一行字符串记忆点。

#### S7-I GUI 焦点（`ai/tools/patch_misc_tools.cpp` 下半）
- `gui_focus_disasm(address)` — **DbgControl**（不是 Write，无 confirm）。`GuiDisasmAt(addr, cip)`（bridgemain.h:1479），第二个参数 cip 是高亮 IP（我们填 addr）。
- `gui_focus_dump(address, index=1)` — DbgControl。`index=1` 走 `GuiDumpAt(va)`，`index∈[2,5]` 走 `GuiDumpAtN(va, index-1)`（SDK 用 0-based）。

### 预设（新增 5 个；总 7 → 12）

| ID | 名 | 核心工具 | 用途 |
|---|---|---|---|
| `crack-license` | 破解许可校验 | get_cfg / set_flag / assemble_at / pattern_replace / list_patches | 定位 strcmp/wcscmp 调用 → 找 gate 跳转 → set_flag 试探 → assemble_at 持久化 |
| `anti-anti-debug` | 反反调试 | locate_api_callers / set_conditional_bp / set_hw_breakpoint / assemble_at | 扫 IsDebuggerPresent/NtQueryInformationProcess/CheckRemoteDebuggerPresent 等 → 用条件断点 force return value，或就地 patch |
| `cfg-explorer` | 控制流图探索 | get_cfg / get_disasm / get_function_range | 纯 Read；强制 systemPrompt 把 mermaid 块放进 ```mermaid 围栏（UI 可渲染） |
| `patch-and-verify` | 补丁与验证 | list_patches / restore_patch / assemble_at / read_memory | 审查补丁、回滚、为新补丁建立"先备份再修改再验证"流程 |
| `trace-input` | 追踪输入数据 | set_hw_breakpoint(type=write) / format_with_dbg / wait_for_event | 用 4 槽硬件写断点跟踪缓冲区被谁写；systemPrompt 强制"用完释放槽位" |

`analyze-function` 同步扩白名单到全 49 工具（保持"全能预设"定位）。

### 设计取舍
- **失败即返、不回滚**（set_conditional_bp）：用户明确选择；理由是 BP_REF 无事务支持，回滚需要先读旧值再恢复（代码翻倍）；`applied` 字段给 LLM 足够信息自我修复。
- **默认 fill_nop=true**（assemble_at）：与 GUI 一致；用户可显式 `fill_nop=false` 关闭。
- **get_cfg 只输出 mermaid**（不返结构化 JSON）：用户确认；理由是 Mermaid 节点定义本身就把 start/end/icount/terminal/icall 都编进 label，AI 完全能从字符串提取；token 翻倍不值得。
- **gui_focus_* 归类 DbgControl 不归 Write**：无副作用、不改任何调试状态；和 wait_for_event 同级，免确认。
- **set_hw_breakpoint 不暴露 size 参数**：SDK 不允许；如果用户想 DR LEN=4 监 DWORD，让 LLM 走 run_dbg_command("bphws 0x.., w, 4")。
- **list_patches 单字节 diff 而非聚合**：保真 SDK 原始数据；4096 上限够 99% 场景（除非有人 patch 整个 .text 段）。

### S7-J 集成
- 4 个新 `.cpp`：`advanced_bp_tools.cpp` (3) / `assembler_pattern_tools.cpp` (3) / `cfg_tool.cpp` (1) / `patch_misc_tools.cpp` (5)，共 12 工具。
- `builtin_tools.h` 加 4 个 register 声明（`registerAdvancedBpTools` / `registerAssemblerPatternTools` / `registerCfgTools` / `registerPatchMiscTools`）；`tool_registry.cpp::registerBuiltinTools` 末尾加 4 个调用。
- `src/CMakeLists.txt` PLUGIN_SOURCES 追加 4 个 .cpp。
- `kPresetSchemaVersion` 10 → 11；`agent_preset.cpp` 新增 5 预设；`analyze-function` 白名单扩 12。

### S7-K 编译验证
- 双架构 Release 编译通过，**零警告零错误**（实测：x64 + x86 各 `warning C` 计数 = 0）。dp64 / dp32 已就位。
- 工具总数：29 读 + 5 控制 + 15 写 = **49 工具**。
- 预设总数：12（freeform / analyze-function / who-calls-here / string-api-context / explain-here / annotate-function / map-program / **crack-license** / **anti-anti-debug** / **cfg-explorer** / **patch-and-verify** / **trace-input**）。
- Git tag：`s7-done`。


## S8：场景化（反调试洞察 / 取证 / SEH / 注入+栈 / trace+错误码+函数注册）（2026-05-24）

### 范围与最终数字

- 新增工具 **14 个**（A=3 反调试洞察 + B=3 取证 + C=1 SEH + D=4 注入/栈 + E=3 trace/错误码/函数）
- 工具总数 **49 → 63**（39 读 + 5 控制 + 19 写）
- 预设 **12 → 14**：新增 `malware-triage` / `unpack-helper`；`anti-anti-debug` 同步扩入 5 个 S8 被动诊断工具
- `kPresetSchemaVersion` **11 → 12**；`analyze-function` 白名单扩到全 63
- Git tag：`s8-done`

### S8-A 反调试洞察（`anti_debug_tools.cpp`）

- `list_threads`：`DbgGetThreadList` → `THREADLIST`；返回每线程的 TID/CIP/SuspendCount/Priority/WaitReason/UserTime/KernelTime/Cycles
  - 关键坑：`THREADLIST.list` 是 C 数组指针，**必须手动 `BridgeFree(list.list)`**（不是 `BridgeList<T>` RAII）
  - `CurrentThread` 是 0-based 数组索引，不是 TID
  - Priority/WaitReason 仅翻译常见枚举（`_PriorityIdle`-15..），其余返回 raw 数值避免误导
- `get_peb_address`：`DbgGetPebAddress`；可选 `thread_id` 用 `DbgGetTebAddress`；可选 `read_bytes` (≤4KB) 同时预读
- `get_anti_debug_flags`：一键诊断
  - BeingDebugged @ PEB+0x02
  - NtGlobalFlag @ PEB + (sizeof(duint)==8 ? 0xBC : 0x68) — **按指针宽度切偏移**
  - ProcessHeap @ PEB + (8字节 ? 0x30 : 0x18)
  - HeapFlags **不硬编码**（Win10/11/版本差异大），返回偏移引导 LLM 自己 read_memory 探
- LLM 优势：纯内存读，**调试器无法被检测**（不调 IsDebuggerPresent 等 API）

### S8-B 取证（`forensic_tools.cpp`）

- `enum_handles`：两阶段 `EnumHandles` 列表 + `GetHandleName(handle, typeBuf, 512, nameBuf, 512)` 补充 type/name
  - `type_filter` 大小写不敏感子串过滤
- `enum_windows`：`WINDOW_INFO`：handle/parent/threadId/style/styleEx/wndProc/enabled/position/title[512]/class[512]
- `enum_tcp_connections`：`TCPCONNECTIONINFO`：local/remote IPv4 + port + 状态字符串
- 全部用 `BridgeList<T>` RAII（取 `operator&()` 自动 Cleanup + 析构 BridgeFree）

### S8-C SEH（`seh_tool.cpp`）

- `get_seh_chain`：`GetSEHChain` → `DBGSEHCHAIN`；`records` 字段需 `BridgeFree` 手动释
- x86 走链表；x64 用 `if constexpr (sizeof(duint)==8)` 返回空 + hint 引导 .pdata/RtlLookupFunctionEntry
- 用 `if constexpr` 替代 `const bool isX64` 规避 C4127 警告（x86 编译时 `isX64` 是编译期常量 false）

### S8-D 注入 + 栈（`injection_stack_tools.cpp`）

- `remote_alloc(addr?, size)`：`Script::Memory::RemoteAlloc`；addr=0 让系统选；**SDK 内部固定 PAGE_EXECUTE_READWRITE**；64MB 上限
- `remote_free(addr)`：只接受 RemoteAlloc 返回的**基址**，不能传中间页
- `stack_push(value)`：`Script::Stack::Push`；ESP/RSP -= ptr_size；返回压栈前的 top + 新 SP（来自 `GetCSP`）
- `stack_peek(offset=0)`：`Script::Stack::Peek`；**offset 单位是 pointer-sized SLOTS 不是字节**（坑！description 标红）
- 决策：**不暴露 `stack_pop`**。真弹出破坏 ESP 一致性，反向工程几乎用不到；要弹出+恢复让 LLM 用 `stack_peek` + `set_register` 显式做
- 关键坑：`Script::Register::GetSP()` 返回 **16 位 SP 子寄存器**！整数栈指针必须用 `GetCSP()`（Current Stack Pointer，duint）

### S8-E trace / 错误码 / 函数注册（`trace_error_func_tools.cpp`）

- `get_trace_record_info(address)`：合并 `GetTraceRecordHitCount` + `GetTraceRecordByteType` + 页对齐后的 `GetTraceRecordType`（None/BitExec/ByteWithExec.../WordWithExec...）；hit_count=0 + record_type=None 时附 hint 提醒先 enable trace record
- `translate_error_code(code)`：0xC0000005 → EXCEPTION_ACCESS_VIOLATION
  - `std::call_once` lazy 灌入全局 `unordered_map<duint, string>`：`EnumErrorCodes` + `EnumExceptions` 合并
  - 后续 O(1) 查找；32-bit 错误码尝试低 32 位回退匹配（负数 errcode 兼容）
  - `CONSTANTINFO.name` 是 dbg 端 `const char*`，立即 `std::string` 拷贝避免悬挂
- `add_function(start, end, manual=true)`：`Script::Function::Add(start, end, manual)`；**end 是最后一条指令 VA inclusive 不是 end+1**；manual=true 防分析器覆盖

### S8-F 集成

- `builtin_tools.h`：新增 5 个 `registerXxxTools` 自由函数声明
- `tool_registry.cpp:40-54`：`registerBuiltinTools()` 末尾追加 5 个调用
- `src/CMakeLists.txt`：追加 5 个 `.cpp` 源文件

### S8-G 预设升级

- `kPresetSchemaVersion` 11 → 12
- `analyze-function` 白名单扩到 63（追加 13 个 S8 工具）
- `anti-anti-debug` 扩入 S8-A 三工具 + S8-B 两工具（enum_handles/enum_windows），systemPrompt 改"先 passive diagnosis 后 active neutralization"流程
- 新增 `malware-triage`（28 工具，maxIter=25）：纯只读 + 仅允许 label/comment 沉淀；workflow = PEB 姿态 → list_threads → enum_handles(全) → enum_tcp → enum_windows(隐藏) → get_seh_chain → memory_map(RWX 私有) → 标注
- 新增 `unpack-helper`（24 工具，maxIter=30）：HW write BP 监控 RWX 区 → run_continue → HW execute BP 抓 OEP jump → `get_trace_record_info` 验真假 OEP → `add_function` 修补分析器 → `stack_peek` 恢复 pushad 上下文
- 决策：原计划的 `anti-debug-bypass` 与 `anti-anti-debug` 语义重叠（一个诊断、一个中和），合并为后者增强而非独立预设

### S8-H 双架构编译

- x64 Release：零警告零错误，`x64dbg_ai_plugin.dp64`
- x86 Release：初始有 1 个 C4127（seh_tool.cpp:91 `if (isX64 && ...)` 在 x86 时 isX64 为编译期常量），改 `if constexpr (sizeof(duint)==8)` 修复后零警告，`x64dbg_ai_plugin.dp32`

### SDK 探查发现（S8 期间）

- ❌ NOT FOUND in SDK：
  - `ValueFromString` — 应用 `ValFromString`
  - `GetPrivilegeList` — SDK 不暴露权限列表 API
  - `VectoredHandler*` — VEH 链无公开 API
  - `enum_constants` 全量 — 只暴露按值查名的 `translate_error_code`
- 已验真签名：
  - `DbgGetPebAddress(bridgemain.h:1236)`：`duint(DWORD pid)`
  - `DbgGetThreadList(:1184)`：`void(THREADLIST*)`，`list.list` 需手动 `BridgeFree`
  - `DbgFunctions()->EnumHandles/GetHandleName/EnumWindows/EnumTcpConnections`（`_dbgfunctions.h:241-258`）
  - `Script::Function::Add(start,end,manual,instructionCount=0)`（`_scriptapi_function.h:19`）
  - `Script::Register::GetCSP()` 才是 duint 栈指针（`_scriptapi_register.h:280`），`GetSP` 是 16 位

---


## S9：工具与预设管理 UI 重构 + 分类系统 + 工具描述中文化（2026-05-25）

### 问题 S9.1：PresetEditor 长 QListWidget 平铺过载
**背景**：S8 后工具数=63、预设数=14，原 PresetEditor 单 QListWidget 平铺所有工具勾选，找特定工具靠肉眼扫；预设列表也是平铺，难以按场景定位。

**方案**（G-10）：
- 数据层加 `group`（工具功能域，与 ToolCategory 正交）+ `tags`（预设多标签）
- 工具勾选：`QListWidget` → `QTreeWidget`（group 节点 + 子工具 + 三态勾选）+ 顶部搜索 + Read/Ctrl/Write chip
- 预设列表：左侧 `QTreeView`（group → 预设）+ 右侧卡片
- `ToolRegistry` 加 `groupOf / listGroups / listToolsByGroup / categoryOf` 4 个 API
- `AgentPreset` schema 12 → 13，加 `group` + `tags`；老配置按 id 推断兜底

### 问题 S9.2：出厂预设可改 → 用户误操作丢失
**方案**：14 个出厂预设全 `readonly=true`；UI 字段全 readOnly + 禁 saveBtn；新增「解锁副本」按钮派生为可编辑用户预设；「恢复出厂」保留所有 readonly=false 用户预设。

### 问题 S9.3：工具按钮 popup menu 信息密度低
原 toolsBtn_ 是 popup menu，只能列工具名。

**方案**：改造为独立只读对话框 `ToolsBrowserDialog`（`src/ui/tools_browser_dialog.{h,cpp}`）：
- 4 列树（工具名 / 类别徽标 / 描述 / 当前预设启用 ✓✗）
- 顶部 badge：`共 N · 当前预设启用 M · 过滤后可见 X 启用 Y`
- 搜索 + Read/Ctrl/Write chip + 「只显示当前预设启用」复选框
- 双击叶子 → 680×560 schema 详情对话框
- 不直接修改预设；底部「打开工作流编辑器…」按钮关闭自身并由 AssistantPanel 调起 PresetEditor
- 类别徽标色：Read=#6FCF97 / Ctrl=#F2C84B / Write=#EB5C5C
- `prettyGroupName/prettyCategoryName` 提升为 `PresetEditorDialog` public static 供复用

### 问题 S9.4：工具描述全英文 → 中文用户上手成本高
**决策（G-9 A 档 · UI only）**：
- `ITool` 加 `virtual std::string descriptionZh() const { return description(); }`（默认 fallback 英文）
- `ChatTool` 加 `descriptionZh` 字段；`ToolRegistry::listChatTools()` 填充
- **LLM 仍读英文 `description()`**：保护 prompt cache + system prompt 英文一致性
- UI（ToolsBrowserDialog + PresetEditorDialog）显示用 `!descZh.empty() ? descZh : desc` 兜底
- 63 工具全部加 `descriptionZh() override`（17 个 `*_tools.cpp` 文件）

### 问题 S9.5：QDialog 子树 dark 主题缺失 → 弹窗白底刺眼
原 QSS 只样式化 `AssistantPanel` 子树。

**方案**：
- `resources/styles/theme_dark.qss` 追加「Dialog form controls」段：QLabel / QLineEdit / QSpinBox / QDoubleSpinBox（自绘箭头） / QCheckBox（自绘 indicator） / QPlainTextEdit / QTextEdit / QComboBox / QGroupBox（标题盖 #252526 背景） / QTreeWidget / QTreeView（三态 indicator） / QHeaderView::section / QDialogButtonBox
- 全 QDialog 前缀作用域，避免污染主面板
- `openPresetManager()` / `openToolsBrowser()` 打开前主动 `setStyleSheet(":/x64dbg-ai/styles/theme_dark.qss")`

### 问题 S9.6：UI 术语不统一（Agent / 预设 / Workflow）
**决策**：UI 层全面改为「工作流」（更贴合用户视角的"任务流程"概念）：
- 主按钮 `" Agent"` → `" 工作流"`
- split menu 「管理预设…」→ 「管理工作流…」
- ToolsBrowserDialog 底部「打开预设编辑器…」→ 「打开工作流编辑器…」
- **内部数据结构 `AgentPreset` / `activePresetId_` / `runAgentWithPreset()` 不动**（涉及 JSON 存档 key + 命名空间，不值得为字面统一冒升级风险）

### 问题 S9.7：「只显示当前预设启用」复选框 disabled 无法勾选
初版逻辑：当 `enabledSet_.empty()`（语义=全部启用）时 disable 复选框，因为"无需过滤"。

**用户反馈**：默认应展示全部，勾选才是过滤，这个 disable 太迷惑。

**修复**（`tools_browser_dialog.cpp:92-98`）：
- 默认 unchecked（=显示全部）
- 只要有激活预设就 enable（`!noActivePreset_`）
- enabledSet 为空时 tooltip 说明「勾选与否结果相同」，UI 不再强制 disable
- Read/Ctrl/Write chip tooltip 也改为「勾选以显示 X，取消则隐藏」更清楚

### 双架构 Release 编译验证
- x64 / x86 均零警告通过
- 工具配对校验：`description() override` 66 处 = `descriptionZh() override` 66 处（含部分文件内辅助类）

---


## S9 后续：场景预设 verdict gate + 新增 sample-triage 预检（2026-05-25）

### 范围与最终数字

- 新增预设 **1 个**：`sample-triage`（exploration 组，read-only + triage）
- 改造预设 **3 个**：`unpack-helper` / `malware-triage` / `anti-anti-debug` 头部加 PHASE 0 verdict gate
- 预设总数 **14 → 15**；`kPresetSchemaVersion` **13 → 14**
- 双架构 Release 编译零警告通过；未打新 tag（挂在 S9 范畴下作为后续优化）

### 问题：场景预设 prompt 前提硬编码导致前提不成立时大量空转

`unpack-helper.systemPrompt` 第一句硬写 "The debuggee is a PACKED executable"，当用户对未加壳样本误选此预设时，agent 会按 7 步 workflow 绕完才反推出"未加壳"结论，浪费 token + iter。`malware-triage` / `anti-anti-debug` 同病。

### 方案 C 落地（用户选定）

#### 1) 新增 sample-triage 预设（形态预判 + 推荐下一步）

- **定位**：只读、轻量、严格预算，只回答 4 个问题：
  - `packed`：加壳判定（节名 UPX*/.aspack/.vmp0/.themida/.petite/.nsp0/.MEW/.MPRESS1 + RWX + 导入稀疏）
  - `anti_debug`：反调试 API 表面
  - `entry_anomaly`：EP 是否在非 .text section
  - `iat_health`：normal/sparse/wiped（按导入条目数）
- **严格预算**：systemPrompt 硬写 `MAX 5 tool calls total`；`maxIter=8`（留迭代余量给 reasoning，工具调用预算自约束）
- **工具集**（10 个，全只读）：list_modules / get_module_info / get_module_imports / get_module_exports / get_memory_map / get_page_protect / get_registers / list_threads / eval_expression / list_labels
- **输出固定 Markdown**：4 维度 checklist + 一行 `recommend: <preset-id>`
- **推荐映射表**写进 prompt：
  - packed=yes → `unpack-helper`
  - packed=no + anti_debug=yes → `anti-anti-debug`
  - packed=no + injection/C2/crypto API 命中 → `malware-triage`
  - 纯净 → `analyze-function`（或想要总览选 `map-program`）
  - 不确定 → `freeform`

#### 2) 三个场景预设加 PHASE 0 verdict gate

每个场景预设头部嵌入 2-3 工具调用的"自检"段，命中场景特征才进入 PHASE 1，否则建议改用 sample-triage / 对应正确预设并 STOP：

- **unpack-helper PHASE 0**（max 3 calls）：get_module_imports（看导入数）+ get_memory_map（看节名/RWX）。无 packer signature → 建议 sample-triage / analyze-function 并 STOP
- **malware-triage PHASE 0**（max 2 calls）：get_module_imports（看可疑 API）+ get_memory_map（看是否仍加壳）。加壳 → 建议 unpack-helper；表面干净 → 建议 sample-triage
- **anti-anti-debug PHASE 0**（max 2 calls）：get_module_imports（看 anti-debug API）+ get_anti_debug_flags（看 PEB 状态）。空表面 + 加壳 → 建议 unpack-helper；空表面 + 干净 → 建议 sample-triage
- 用户可显式说 "skip triage" / "I already confirmed it is X" 跳过 gate（写进 PHASE 0 文本作为 escape hatch）
- **unpack-helper enabledTools** 补 `get_module_imports / get_module_exports / get_module_info`（PHASE 0 必备）；其它两个预设原本已含

### 设计取舍

- **不做 sub-agent 框架**：理论上可让 sample-triage 作为 unpack-helper 的子 agent 自动跑 PHASE 0，但当前 AgentLoop 无 sub-agent 机制（新增框架违反"最小改动"），且 inline PHASE 0 共享 prompt cache + 单轮对话体验更好
- **PHASE 0 与 sample-triage 共存**：场景预设的 PHASE 0 是 sample-triage 的"嵌入精简版"，专攻自己的前提；sample-triage 作为独立可选预设让用户主动选完整 4 维度预检
- **保留 malware-triage 不动**：malware-triage（行为分诊）与 sample-triage（形态预判）目标不同，不合并
- **新预设组 = exploration**：而非新开 "triage" 组，原因是只多一个预设单开一组冗余；triage 作为 tag 区分即可
- **VERDICT FORMAT 写死在 prompt**：用 `\n` 转义嵌入字符串字面量，确保 LLM 输出可被未来的 UI 解析器（若有）规范处理

### 关键文件变更

- `src/ai/agent_preset.cpp`：
  - 第 426-450 行（anti-anti-debug）：systemPrompt 头部插 PHASE 0
  - 第 585-609 行（malware-triage）：systemPrompt 头部插 PHASE 0
  - 第 644-693 行（unpack-helper）：systemPrompt 头部插 PHASE 0；enabledTools 加 get_module_imports/_exports/_info
  - 第 718-755 行：新增 sample-triage 预设定义
  - kMeta 新增 `{"sample-triage", {"exploration", {"read-only", "triage"}}}`
- `src/ai/agent_preset.h:30`：`kPresetSchemaVersion = 14`
- 文档：features.md（schema 13→14、预设 14→15、出厂预设表重排）；skills-roadmap.md（新增 §5.6）；development-log.md（本节）

### 验证

- 双架构 Release 编译：x64 + x86 各 0 警告 0 错误，dp64 / dp32 已就位
- Runtime 实测：留待真实样本回归（未加壳样本走 unpack-helper PHASE 0 应在 ≤3 calls 内返回"建议改用 sample-triage"）


## G-2：prompt cache 命中观测（2026-05-25）

### 背景

DeepSeek / Copilot 都支持 prompt caching（system prompt + tools schema 命中后 input token 价 ÷10），但此前无任何观测手段——既不知道是否真有命中，也不知道命中率多少。这导致两个下游问题悬而未决：
1. G-9 B 档（userTemplate 中文化）会不会破坏 cache 没法量化判断
2. 优化 system prompt / tools schema 排序的方向是盲改

### 关键勘察发现

- `chat_provider.h::UsageInfo` 原本只有 prompt/completion/total/reasoning 4 字段，**没有 cache 相关字段**
- DeepSeek 流式分支 **未传** `stream_options.include_usage=true`——按 OpenAI 协议这意味着 SSE 末尾不会发 usage chunk，所以流式调用根本拿不到 token 数
- DeepSeek 非流式分支 `response.usage` 已被读取（line 225 附近），但**只读了 prompt_tokens/completion_tokens**，未读 `prompt_cache_hit_tokens`
- Copilot client 同样未传 `include_usage`，且字段名取决于底层模型（OpenAI 风格 vs Anthropic 风格 vs DeepSeek 风格），需运行时兼容
- `AgentLoop::AgentRunCallbacks` 与 `AgentWorker` 信号系统都没有 usage 通道

### 实施

#### 1) 数据通路（chat_provider.h）

```cpp
struct UsageInfo {
    int promptTokens = 0;
    int completionTokens = 0;
    int totalTokens = 0;
    int cachedPromptTokens = 0;
    int cacheCreationTokens = 0;  // Anthropic 写入 cache 的成本
    int reasoningTokens = 0;
    double hitRatio() const {     // -1 表示 promptTokens=0 无法计算
        return promptTokens > 0
                 ? double(cachedPromptTokens) / double(promptTokens)
                 : -1.0;
    }
};
```

`ChatStreamCallbacks` 加可选 `std::function<void(const UsageInfo&)> onUsage`。

#### 2) DeepSeek client（`deepseek_chat_client.cpp`）

- 流式 body 加 `"stream_options": {"include_usage": true}`
- SSE parser 在 `nlohmann::json::parse(data)` 后先检测 `j.contains("usage")` 而不是直接走 `choices`——usage chunk 的 `choices` 字段是空数组，原 `if (!j.contains("choices") || j["choices"].empty()) return;` 正好把它过滤掉了
- 非流式分支也补上 cache 字段解析

#### 3) Copilot client（`copilot_chat_client.cpp`）

抽 `parseUsage(const nlohmann::json& u) -> UsageInfo` helper 兼容三套字段命名：

| Provider | input | cached | reasoning | cache_creation |
|---|---|---|---|---|
| DeepSeek | `prompt_tokens` | `prompt_cache_hit_tokens` | `completion_tokens_details.reasoning_tokens` | — |
| OpenAI (Copilot) | `prompt_tokens` | `prompt_tokens_details.cached_tokens` | `completion_tokens_details.reasoning_tokens` | — |
| Anthropic (Copilot) | `input_tokens` | `cache_read_input_tokens` | — | `cache_creation_input_tokens` |

Helper 先按 OpenAI 命名读，0 时 fallback Anthropic 命名；cache 字段双路径都试一遍。

#### 4) AgentLoop / AgentWorker 透传

- `AgentRunCallbacks::onUsage` 加进结构体
- `agent_loop.cpp` 在构造 `ChatStreamCallbacks` 时把 `scb.onUsage = cb.onUsage`
- `AgentWorker` 加 signal：

```cpp
void usageUpdated(int promptTokens,
                  int cachedPromptTokens,
                  int completionTokens,
                  int reasoningTokens,
                  double hitRatio);
```

- 工作线程内通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 派发到 UI 线程

#### 5) 日志

每次 LLM 调用结束写单行 `[G-2 CACHE]`：

```
[G-2 CACHE] input=12450 cached=11968 miss=482 hit_ratio=96.1% completion=287 reasoning=0 cache_creation=0
```

`prompt_tokens=0` 时记 `hit_ratio=n/a`，避免除零。

#### 6) AssistantPanel 显示

- 加成员 `QString lastCacheStatus_` 缓存最近一轮拼好的字符串
- `setActivePreset()` 在 label 末尾追加 `· input=N cache=N%`（若 `lastCacheStatus_` 非空）
- usageUpdated signal handler 更新 `lastCacheStatus_` 并 `setActivePreset(activePresetId_)` 重画
- `agentStatusLabel_->setToolTip` 写完整 4 字段（input / cached / hit_ratio）

### 关键设计取舍

- **流式必须 include_usage**：否则只能在最后一次非流式调用拿到 usage，多轮 agent loop 中间轮全是黑盒
- **三套字段命名兼容**：因为 Copilot 后端可能路由到任何模型；DeepSeek 也兼容写进 helper 是为了未来可能复用
- **不新增独立 label**：直接拼在 agentStatusLabel_ 末尾，避免顶栏宽度增加；细节走 ToolTip
- **不打 tag**：单点改进，挂在 S9 之后，下次 milestone 时再统一打

### 验证

- 双架构 Release 编译：x64 + x86 各 0 警告 0 错误
- Runtime 实测留待回归，将根据真实 cache hit ratio 决定 G-9 B 档（userTemplate 中文化）是否值得做
