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
