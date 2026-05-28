# 已知问题与技术债

按严重程度分组。所有条目都不影响主功能可用，但建议后续迭代逐步消化。

---

## 🟡 中等优先级（影响体验或可维护性）

### K-01：老 DB 无 EXE 路径元信息 ✅ 已修复
- **状态**：已在 M3.6 之后引入 `meta` K-V 表
- **实现**：
  - `SessionStore` 加 `setMeta / setMetaIfAbsent / getMeta`（线程安全）
  - `storage/meta_keys.h` 定义标准键：`exe_path / exe_filename / first_seen / last_seen`
  - `ProjectContext::onDebugStart` 计算 SHA 后写入：exe_path / exe_filename 每次覆盖，first_seen 用 `INSERT OR IGNORE`，last_seen 每次覆盖
  - `ProjectBrowser::listProjectDbs()` 顺手 ro 打开每个 db 读 meta（容错：表不存在/老库时留空）
  - `HistoryDialog` 左栏：有 filename 时显示 `vmprotect_demo.exe · SHA前8`，否则回退到 SHA12；tooltip 含完整 path + first/last seen
- **历史遗留**：老 db（没写过 meta）继续显示 SHA，不影响功能

### K-02：0 KB 空 DB 累积 ✅ 已修复
- **状态**：已在 `pluginit` 启动时清理
- **实现**：`ProjectBrowser::cleanupEmptyDbs(minAgeDays = 7, activeSha = {})`
  - 规则 1：文件 size==0 → 直接删
  - 规则 2：size>0 但 `sessions` 与 `chunks` 两表均为空（或表不存在）且 mtime 早于 now-7 天 → 删
  - 同时清理同名 `-wal / -shm / -journal` sidecar
  - 当前活动 db 永不动（pluginit 阶段 activeSha 为空，由"7 天"规则间接保护）
- **触发点**：`plugin/plugin_main.cpp` pluginit 末尾同步执行（IO 量小，<10 ms）

### K-03：Config 只读不写
- **现象**：`config.json` 只解析不回写。用户在 UI 里改的设置（HTTP 超时、日志级别）下次启动丢失
- **位置**：`src/util/config.cpp`
- **方案**：加 `Config::save()`，UI 改设置时调；nlohmann-json `dump(2)` 写文件即可
- **影响**：低，目前没有暴露 UI 改设置的入口

### K-04：QStyle 未显式 include ✅ 已修复
- **状态**：已在 docs 巡检后补上 `#include <QStyle>`
- **位置**：`src/ui/assistant_panel.cpp` 头文件块（按字母序插在 `QStringList` 与 `QToolButton` 之间）
- **背景**：原靠间接 include（其他 Qt 头带的）能编过，换 Qt 版本/重排 include 可能崩

### K-05：stackTree 启动后未默认展开 ✅ 已修复
- **状态**：已在 `TraceDialog::rebuildStackTree()` 末尾追加 `stackTree_->expandToDepth(2)`
- **位置**：`src/ui/trace_dialog.cpp:604`
- **效果**：CallStack 模式采样完成后默认展开两层 trie，常见调用链一眼可见，无需逐节点点开

### K-06：spdlog 末尾日志可能未刷盘
- **现象**：x64dbg 崩溃或强制结束时，最后几条日志可能丢
- **缓解**：
  - 不要全程 `flush_on(info)`（性能损耗大）
  - 在关键路径（崩溃前 / plugstop）显式 `spdlog::default_logger()->flush()`
  - 已在 `plugstop` 加 flush，但 CB_STOPDEBUG 等其他路径可考虑也加

---

## 🟢 低优先级（小毛病/可有可无）

### K-07：嵌套 table 气泡 HTML
- **现象**：ChatView 气泡布局用嵌套 `<table>` 实现；M3.4 流式增量已规避了此带来的性能问题，但**全量 rerender 仍走 setHtml(整文档)**
- **触发**：切换会话 / 加载长历史会话时一次性 setHtml ~MB 级 HTML，可能短暂卡 100-300 ms
- **彻底方案**：把气泡改成纯 `<div>` + flex 布局；保留嵌套 table 是因为旧 Qt 5.12 QTextDocument 对 div+flex 渲染不一致
- **不修原因**：日常单次 rerender 触发频次低；流式路径已走增量

### K-08：ICU 找不到的 warning
- **现象**：CMake 配置时偶尔报找不到 ICU 组件
- **影响**：无（Qt5 静态构建不强依赖 ICU 完整组件）
- **不修原因**：纯日志噪声

### K-09：DeepSeek reasoner 思考链显示样式 ✅ 已修复
- **状态**：已在 M4.6 Reasoning UI 解决
- **实现**：
  - `IChatProvider` 扩展 `ChatStreamCallbacks.onReasoningDelta` + `ChatMessage.reasoningContent` 独立通道
  - `ChatView` 新增 `ReasoningBlock`：QToolButton 折叠按钮 + 灰色等宽 QLabel，默认折叠
  - 标题流式更新 `▶ 思考过程 (N)` 显字符数；完成后保留
  - 与下方助手气泡分离的 widget，得益于 M4.6c ChatView 重构（QScrollArea + VBox 子控件流）
- **遗留**：未提供"复制思考过程"按钮（见 K-14）

### K-10：CallStack 系统模块清单需要维护
- **位置**：`src/trace/call_graph.cpp` `kSystemMods`
- **现象**：约 30 个常见系统模块名硬编码，新 Windows 版本可能引入新模块（如 `windows.storage.dll`）
- **方案**：长期可改为按 `C:\Windows\System32\` 实际存在的 DLL 名动态判断；当前列表对常见场景够用

### K-11：Locator API 名前缀启发式不够
- **现象**：用户输 `send` 时，自动 fallback 试 `_send` / `__imp_send`；但有些库导出名是装饰过的（`?send@xxx@@YAXXZ`）
- **方案**：增加 undecorate 步骤（`UnDecorateSymbolName`）；当前限于纯 C 导出

### K-12：trace_demo 仅覆盖简单流水线
- **现象**：用 4 个 NOINLINE 函数串成的演示程序回归 trace；缺真实虚函数 / 多线程 / 异常路径回归
- **方案**：后续加 trace_demo2，包含 std::thread / try-catch / 虚表调用，验证调用图折叠的边界条件
- **不修原因**：当前 trace 真实程序（JX3ClientX64）已能正常工作

### K-13：AgentWorker setAgentRunning(false) 重复调用 ✅ 已修复
- **状态**：已在 AssistantPanel 加去重标志
- **实现**：
  - `AssistantPanel::agentTerminalEventHandled_` 布尔成员；每次 `runAgentWithPreset` 入口重置为 false
  - `failed` / `maxIterReached` 信号处理路径里置 true（这两个是"终态先到"路径）
  - `finished` 信号处理路径：检查 flag，若 true 则跳过 `XAI_LOG_INFO("agent finished ...")` 这条冗余日志，但 `setAgentRunning(false)` 仍调一次保险（幂等）
- **位置**：`src/ui/assistant_panel.{h:154,cpp:1125,1195,1209,1215}`
- **效果**：plugin.log 不再因 failed/maxIter 路径出现 "agent finished" 重复条目

### K-14：Reasoning UI 缺"复制思考过程"按钮
- **现象**：ReasoningBlock 展开后是 QLabel，用户无法选中 / 复制思考链文本
- **方案**：QLabel 改 `setTextInteractionFlags(Qt::TextSelectableByMouse)`；折叠按钮右侧加 📋 复制按钮
- **影响**：低；用户可在 plugin.log 里找全文
- **位置**：`src/ui/chat_view.cpp` ReasoningBlock 定义

### K-15：CB_STOPDEBUG 双注册（C-1）✅ 已修复
- **状态**：S0 修复，详 development-log §S0-C1
- **位置**：`src/plugin/plugin_callbacks.cpp:29-56`、`src/trace/trace_recorder.cpp:47-49,65-85`

### K-16：cbInitDebug 同步 SHA256 阻塞 UI（C-2）✅ 已修复
- **状态**：S0 修复，detach 线程 + generation 抢占语义，详 development-log §S0-C2
- **位置**：`src/storage/project_context.{h,cpp}`

### K-17：tools 入参拒绝字符串化数字（H-1）✅ 已修复
- **状态**：S0 修复 `read_memory.size` / `get_disasm.lines`；dynamic_context / static_analysis 的可选 hint 参数走默认值兜底，S1 顺手统一
- **位置**：`src/ai/tools/basic_read_tools.cpp::parseInt32Lenient`

### K-18：PresetStore::save 偶发 rename "Access is denied"
- **现象**：S0+S1 烟测冷启动 + schema v3→v5 迁移时，`MoveFileExW(tmp → final, MOVEFILE_REPLACE_EXISTING)` 偶发失败；in-place 覆写 fallback 立刻成功，5 个 preset 正常落盘
- **复现**：低频，仅在 `agent_presets.json` 升级 schema 时出现一次；正常使用未观察到
- **疑因**：杀软/Defender 实时扫描 `.tmp` 持锁；或上次进程残留句柄；或 OneDrive 等同步软件抢占 Roaming 目录
- **影响**：无（fallback 路径已覆盖）
- **方案**：后续可改为 `ReplaceFileW` + 3 次 50/100/200ms 退避重试；当前 in-place fallback 已可靠
- **位置**：`src/storage/preset_store.cpp::save`

### K-19：run_until 的 one-shot 走 DbgCmdExec 而非 Script API
- **现象**：`run_until(addr)` 实现走的是 `DbgCmdExecDirect("bp 0x.., ss")` 命令文本，不是 `Script::Debug::SetBreakpoint`
- **原因**：x64dbg Script::Debug API 没暴露 singleshoot 标志，必须用命令字符串里的 `ss` 选项
- **副作用**：依赖命令解析的稳定性；命令拼接前已 `std::format("0x{:x}", addr)` 规范化，理论 OK
- **风险**：x64dbg 升级时若变更 `bp <addr>, <option>` 语法（极低概率），需同步更新
- **影响**：低；timeout 路径已有 `DeleteBreakpoint` 兜底清理
- **位置**：`src/ai/tools/debug_write_tools.cpp::RunUntilTool::invoke`

### K-20：run_dbg_command 白名单按"首 token"判定
- **现象**：仅检查命令首个 token 是否在 15 token 白名单内（按空白/逗号切，转小写）
- **副作用**：`bp 0x401000, ss` 这种带选项的命令首 token 是 `bp`，能过白名单；但参数部分不做语义检查，理论上可被 prompt injection 构造 `bp 0x401000; <malicious>` 之类
- **缓解**：x64dbg 命令解析本身不支持 `;` 串行执行；多命令需要换行或 script，单次 `DbgCmdExecDirect` 一次只发一行
- **影响**：低；但严格起见建议未来加参数 sanity check（地址范围 / 模块名格式 etc）
- **位置**：`src/ai/tools/debug_write_tools.cpp::RunDbgCommandTool::isWhitelisted`

### K-21：ToolConfirmDialog 跨线程 BlockingQueuedConnection 死锁风险
- **现象**：write 工具在 worker 线程调 `confirmFromBackground` → `invokeMethod(app, lambda, BlockingQueuedConnection)` 切 GUI 线程并阻塞等返回
- **风险**：若 GUI 线程正在等 worker 线程（如 `AgentWorker::waitForFinished`），双向等待会死锁
- **当前规避**：`AgentWorker` 用 `QtConcurrent::run` 跑后台，GUI 线程不阻塞等 worker；工具调用过程中 GUI 线程只跑事件循环
- **若引入 wait_for_event 嵌套 + 取消按钮路径**：需确保取消信号通过 `cancelFlag.store(true)` 非阻塞投递，不在 GUI 线程做 join
- **影响**：低；当前架构安全。但后续若加"批量执行多个工具的进度对话框 + 取消"需特别小心
- **位置**：`src/ui/tool_confirm_dialog.cpp::confirmFromBackground`

### K-22：run_script_file 无法同步等待脚本结束（S5）
- **现象**：x64dbg SDK 的 `DbgScriptRun(destline)` 异步触发脚本执行，没有"finished" 事件 / 回调
- **后果**：`run_script_file` 只能 fire-and-forget；agent 调完该工具立刻返回 started=true，无法在工具结果里反映脚本是否成功
- **缓解**：description 明确告知 agent 用 `wait_for_event(Paused/Breakpoint)` 或后续 `get_registers/read_memory` 观察副作用
- **若脚本死循环**：需用户手工在 x64dbg Script 标签页按 Abort，或 agent 调 `run_dbg_command("StopDebug")`
- **影响**：中；要求 agent prompt 里教会 LLM 这个模式（已在 system prompt 的 tool description 体现）
- **位置**：`src/ai/tools/script_tools.cpp::RunScriptFileTool`
- **可能改进**：x64dbg 有 `CB_SCRIPTFINISHED` 之类回调吗？需进一步调研 `_plugin_registercallback` 列表

### K-23：场景预设 PHASE 0 verdict gate 与 maxIter 计算共享预算
- **现象**：S9 后续方案 C 给 unpack-helper / malware-triage / anti-anti-debug 加 PHASE 0 自检（2-3 calls），但这些 calls 也计入 `maxIter`
- **后果**：原本 maxIter=30 的 unpack-helper 现在 PHASE 1 实际可用 27 轮；复杂样本若 PHASE 0 误判向 PHASE 1 推进，进 PHASE 1 后预算偏紧
- **缓解**：PHASE 0 严格限制 max 3 calls 且无误判时主动 STOP；已通过 systemPrompt 硬约束
- **影响**：低；尚无 runtime 出现 maxIter 触底案例
- **位置**：`src/ai/agent_preset.cpp:426-693` 三个场景预设
- **可能改进**：AgentLoop 加 `phase0BudgetCalls` 字段独立计数，留待 G-3（`maxToolCalls` 与 maxIter 分离）一并做

### K-24：Copilot prompt cache 命中数依赖底层模型路由（G-2 发现）
- **现象**：Copilot 后端可能路由到 OpenAI / Anthropic / 其它模型，usage 字段命名不一；部分模型可能完全不返回 cache 字段
- **后果**：AssistantPanel 顶栏可能显示 `input=N cache=n/a` 或 `cache=0%`，不一定是 cache 失效而是模型不暴露字段
- **缓解**：`parseUsage()` helper 已三套字段兜底；`hitRatio() < 0` 时日志写 `hit_ratio=n/a` 与 `=0%` 区分
- **影响**：观测层；不影响 cache 本身是否生效
- **位置**：`src/ai/copilot_chat_client.cpp::parseUsage`
- **后续**：runtime 实测拿到不同模型样本后建文档表

### K-25：`stream_options.include_usage=true` 对非官方 OpenAI 兼容端点可能报错
- **现象**：G-2 给 DeepSeek / Copilot 流式 body 加 `stream_options.include_usage=true`，但 OpenAI 协议规定该字段不被所有兼容端点支持
- **后果**：若用户改 `api_base` 指到不支持此字段的反代 / 开源 OpenAI 兼容服务（如 vLLM 部分版本），可能 400 报错
- **缓解**：暂无；DeepSeek 官方 + GitHub Copilot 实测正常
- **影响**：低；当前仅这两个 provider 支持，无第三方端点配置
- **位置**：`src/ai/deepseek_chat_client.cpp:136` + `src/ai/copilot_chat_client.cpp:55`
- **可能改进**：捕获 400 后自动关闭 include_usage 重试

### K-26：`agentStatusLabel_` 文本拼接 cache 状态可能超出顶栏宽度
- **现象**：G-2 在 `[预设名]` 后追加 `· input=N cache=N%`，长预设名如 `anti-anti-debug` + 长 cache 数字可能挤压旁边按钮
- **后果**：低分屏 / 窄面板下可能换行或截断
- **缓解**：完整数据在 ToolTip 里；label 本身用 `Qt::ElideRight` 自然截断
- **影响**：极低；尚无用户反馈
- **位置**：`src/ui/assistant_panel.cpp::setActivePreset`
- **可能改进**：cache 状态独立小 label，或顶栏改两行布局

### K-27：中文路径（被调程序 / 用户名 / APPDATA）全链支持 ✅ 已修复
- **状态**：S3 (2026-05-26) 边界转码 + 内部 UTF-8 方案落地
- **真正根因**：x64dbg SDK 的 `char*` 路径**本来就是 UTF-8**（`bridgemain.h:1477` 明确写 "code page is utf8"）。问题在于 **MSVC `std::filesystem::path(std::string)` 把入参按 ACP 解码**，UTF-8 的中文字节会被当 GBK 误读 → 路径乱码 → `fs::exists` 失败 → `ProjectContext::store` 永远 nullptr → "未在调试" / 历史浏览也读不到
- **历史错判（已纠正）**：第一版修复以为 SDK 是 ACP，对 `szFileName` 强转 `ansiToUtf8`，反而把 UTF-8 当 GBK 二次解码，路径更乱（修了又坏）。第二版改为透传 + 内部 UTF-8、只在 `isValidUtf8` 失败时兜底 ansiToUtf8 才彻底好
- **症状（修前）**：`logs/plugin.log` 出现 `main module path not exist: E:\gongju\jw???...\LgExe.exe`
- **实现**：
  - 新增 `util/encoding.{h,cpp}`：`ansiToUtf8 / utf8ToAnsi / wideToUtf8 / utf8ToWide / fsPathFromUtf8 / fsPathToUtf8 / isValidUtf8`
  - `plugin/plugin_callbacks.cpp::cbInitDebug` 直接透传 SDK 字符串（不再强转 ANSI）
  - `storage/project_context.cpp` 全链改用 `fsPathFromUtf8`（内部走 `wstring` 构造，绕开 MSVC ACP 解码坑）；后台 SHA 线程装好 store 后 `EventBus::publish(DbgEvent::ProjectStoreReady)`
  - `storage/project_context.cpp::GetMainModulePath` 兜底分支也透传 + `isValidUtf8` 校验
  - `util/paths.cpp` 从 `_dupenv_s` 切到 `_wdupenv_s`，避免中文用户名 / 中文 `APPDATA` 在转 string 时被截断
  - `storage/session_store.cpp::open` sqlite3_open 用 `fsPathToUtf8`；构造尾部对旧库 `exe_path/exe_filename` 做 ACP→UTF-8 幂等迁移
  - `storage/project_browser.cpp::openReadOnly` URI 路径走 `wideToUtf8(path.wstring())`
  - `util/logging.cpp` spdlog 路径用 `utf8ToAnsi(fsPathToUtf8(...))` 折中（不引 `SPDLOG_WCHAR_FILENAMES` 宏避免全项目签名牵连）
  - `ui/assistant_panel.cpp` 订阅 `DbgEvent::ProjectStoreReady` → marshal 到 GUI 线程刷新状态栏 + 会话列表
- **边界遗留**：
  - LLM 工具结果 / Agent stdout 中的中文字符串若途经 SDK ANSI API（罕见），仍可能乱码（留 S4 单独审查）
  - 用户名包含 ACP 无法表示的 Unicode 字符时，spdlog 日志路径会失败（极少见；fallback 静默无日志）
- **决策依据**：见 `decisions.md` 2026-05-26 条目（边界转码 vs 全 wchar）

### K-28：`patch_memory` 走 DbgMemWrite 导致补丁不可见、不可撤销、不可导出 ✅ 已修复
- **状态**：2026-05-26 同批改动 + 新增 `patch_file` 工具
- **现象**：用 `patch_memory` 修改字节后，`list_patches` 看不到这条补丁，`restore_patch` 也撤不回；后续若要把所有补丁导出为 patched.exe 也漏掉这部分
- **根因**：`data_write_tools.cpp::PatchMemoryTool::invoke` 调用的是 `DbgMemWrite`（朴素 WriteProcessMemory），它不会在 x64dbg 内部 Patch tracker 里登记；只有 `DBGFUNCTIONS->MemPatch / AssembleMemEx / SearchAndReplaceMem` 会
- **副作用**：与 `assemble_at` / `pattern_replace` 走的写路径不一致 — 后两者补丁可见、`patch_memory` 不可见，LLM 用得越多越混乱
- **修复**：
  - `patch_memory` 改走 `DbgFunctions()->MemPatch`，与其余写工具一致
  - description 顶上明说"会被 list_patches 看到 / restore_patch 撤销 / patch_file 导出"，减少 LLM 误用
- **配套**：补齐缺失的最后一环 — 新增 `patch_file` 工具（`patch_misc_tools.cpp::PatchFileTool`），走 SDK `DBGFUNCTIONS->PatchFile`，支持按 module / addresses 过滤导出，等价 x64dbg GUI 的 File → Patch file...
- **影响**：完整破解 / 打补丁工作流（patch 字节 → 列表审查 → 撤销不要的 → 导出 patched.exe）首次端到端打通

### K-29：`search_pattern` 带 module 参数失败 / 工具失败时 agent 看不到 error ✅ 已修复
- **状态**：2026-05-27
- **现象 1（search_pattern）**：调 `search_pattern(pattern="48 8B ?? ??", module="LgExe.exe")` 返回 ok=false `eval failed: mod.size("LgExe.exe")`
- **根因**：原实现走 `DbgEval("mod.size(\"name\")")` 算搜索区间长度，但 x64dbg 表达式求值器**不接受带引号的字符串参数**，永远 fail；同样写法 `mod.base()` 也不行
- **修复**：`static_analysis_tools.cpp:325-336` 改用 `DbgFunctions()->ModSizeFromAddr(base)`（base 已由 `DbgModBaseFromName(name)` 拿到），不再走表达式层
- **现象 2（agent 看不到 error）**：所有 69 个工具，凡是返回 ok=false 的，agent 拿到的 ToolResult 都只看到一个空 data；plugin.log 里也只打 `tool=xxx ok=false` 没具体 error
- **根因**：`agent_loop.cpp:165` 只在 ok=true 路径打了 result，ok=false 路径忘记打 `tr.error`
- **修复**：ok=false 时加 `XAI_LOG_WARN("tool {} failed: {}", name, tr.error)`，69 工具共享受益
- **影响**：诊断工具失败现在有直接证据；search_pattern + module 路径打通
- **位置**：`src/ai/tools/static_analysis_tools.cpp:325-336`、`src/ai/agent_loop.cpp:165-172`

### K-30：系统 API 软断 / 硬断都会冻结操作系统 ✅ 已修复
- **状态**：2026-05-27 两轮：第一轮挡 set_breakpoint，第二轮补 set_hw_breakpoint
- **场景**：调试外挂程序 `���PVP.exe`，LLM 自主下断 `bp kernel32.LoadLibraryW` 排查注入，触发后**鼠标键盘全卡死，整个 Windows 桌面无响应**
- **物理根因**（之前判断硬件断点"安全"是错的）：
  - 被调试进程持有**全局键鼠 hook**（SetWindowsHookEx / Raw Input / 注入到 explorer / 反作弊心跳），它一被 x64dbg 暂停，hook 链上所有进程的输入都卡
  - 外挂频繁 LoadLibrary / VirtualAlloc / EnterCriticalSection（百次/秒级），每次命中 x64dbg 处理 INT3 几十 ms，叠加暂停-恢复反复抖动 → 即使 run_continue 也"停不下来"（实测 17:03-17:04 plugin.log，run_continue 15 秒超时未停）
  - **硬件执行断点也不安全**：DR0-DR3 不修改内存（不会触发反作弊 0xCC 校验），但每次命中仍暂停整个被调试进程所有线程，外挂的 hook 一样卡桌面
- **修复**：`debug_write_tools.cpp::SetBreakpointTool` + `advanced_bp_tools.cpp::SetHwBreakpointTool` 都加同套防护
  - `resolveBpAddr`：先 parseVa，失败 fallback `DbgEval`，支持 `kernel32.LoadLibraryW` 直接当 address（之前 LLM 必须先 eval_expression 拿 VA 再下断，绕路）
  - `isSystemModule`：21 个系统 DLL 白名单（ntdll/kernel32/kernelbase/user32/gdi32/advapi32/ws2_32/...）
  - `isHighFreqApi`：~50 个高频 API 黑名单（LoadLibrary*/Virtual*/Create*/Wait*/Peek*/Sleep*/EnterCriticalSection/QueryPerformanceCounter/...）
  - `classifyBpAddr`：用 `DbgGetModuleAt` + `DbgGetLabelAt` 反查地址所在模块和符号；系统模块 + 热 API → dangerous=true
  - SetBreakpointTool（软件断点全 type）+ SetHwBreakpointTool（仅 execute 类型）命中 dangerous → 返回 REFUSED + 明确的 3 个替代方案：
    1. `set_conditional_bp` 加 `arg.get(0)` 等过滤条件（命中后 LLM 评估条件，不满足自动 resume，避免暂停被调试进程）
    2. 调用方下断（`find_xrefs_to` / `locate_api_callers` 先找被调试模块内的 caller 再下断）
    3. （明确**不再**推荐 type=hardware，第一轮文案是错的）
  - 成功时回写 `module` / `symbol` 字段方便 LLM 知道下到了哪
  - description 英中两版都加显式"freeze OS"警告，教 LLM 不要试图绕过
- **未挡的路径**（已评估，故意放行）：
  - `set_hw_breakpoint` type=write/access（数据断点对系统 API 入口很少用，且不是行执行频率）
  - `set_conditional_bp`（条件断点本身就是给热 API 设计的安全方案）
  - `run_dbg_command` 的 `bp` / `bpx` / `SetBPX` 命令（白名单工具，已要求 5s confirm；后续若实测有人绕过再补 token 解析）
- **影响**：调试反作弊 / 外挂 / 全局 hook 类目标时不再因 LLM 自主下断系统 API 而冻结桌面
- **位置**：`src/ai/tools/debug_write_tools.cpp`（SetBreakpointTool + 共享判定函数）、`src/ai/tools/advanced_bp_tools.cpp`（SetHwBreakpointTool + 同套判定函数的本地副本）
- **技术债**：`resolveBpAddr` / `isSystemModule` / `isHighFreqApi` / `classifyBpAddr` 在两个 .cpp 各有一份副本；后续若加第三个断点类工具（如未来的 trace bp）应抽到 `tool_args_util.h` 或新建 `bp_safety.h`

### K-31：LLM 无法启动 / 重启 / 附加 / 脱离 / 结束调试会话 ✅ 已修复
- **状态**：2026-05-27
- **现象**：LLM 尝试 "启动调试" 或 "重启调试" 都失败；走 `run_dbg_command("InitDebug ...")` 被拒（白名单不含），换走名字猜的工具（`start_debug` / `restart_debug`）发现根本不存在
- **双重根因**：
  1. `run_dbg_command` 白名单（`debug_write_tools.cpp:205`）只放行 bp/bpc/bphwc/bpd/bpe + run/StepInto/StepOver/StepOut/pause + db/dw/dd/dq，**完全没有** `InitDebug` / `init` / `Restart` / `attach` / `detach` / `StopDebug` / `stop`
  2. 即使白名单放行，`RunDbgCommandTool::invoke` 顶上有 `if (!ctx.debuggerActive || !DbgIsDebugging()) return error("debugger is not active")`，**未调试时所有 run_dbg_command 都拒** —— 而 start_debug / attach 本来就是在未调试时调用的，永远过不去
- **设计取舍**：会话生命周期类操作不放进 run_dbg_command 白名单，原因：
  - 这五个操作语义差异大、危险等级不一（start 起新进程 / restart 杀重启 / detach 让进程裸跑 / stop 杀进程），不该塞在一个泛型逃生口里
  - 每个都有特定参数（target_path / pid），用 schema 化的命名工具比 raw command 字符串更稳；LLM 也不容易拼错命令语法
- **修复**：在 `debug_write_tools.cpp` 末尾新增 5 个专用工具，category=Write，requiresUserConfirmation=true（走 5s 倒计时弹窗）：
  | 工具 | x64dbg 命令 | 守卫 | 备注 |
  |---|---|---|---|
  | `start_debug(target_path, command_line?)` | `InitDebug "path"[, cl]` | 已有会话 → 拒 | path 强加引号防空格切断 |
  | `attach_debug(pid)` | `attach <hex_pid>` | 已有会话 → 拒 | x64dbg 数值默认 hex |
  | `detach_debug()` | `detach` | 无会话 → 拒 | 进程继续裸跑 |
  | `restart_debug()` | `Restart` | 无会话 → 拒 | 断点保留 |
  | `stop_debug()` | `StopDebug` | 无会话 → no-op | 不算错 |
- **工具总数**：69 → 74
- **影响**：脱壳 / 多次取证 / attach 外部进程等场景下，LLM 可自主管理会话生命周期；之前必须用户手工操作 x64dbg GUI
- **位置**：`src/ai/tools/debug_write_tools.cpp::StartDebugTool/AttachDebugTool/DetachDebugTool/RestartDebugTool/StopDebugTool` + 同文件 `registerDebugWriteTools` 末尾 5 行注册
- **未做**：LLM 不能"启动调试到指定 OEP 后自动 wait_for_event"做成 atomic 工具 —— 留给 LLM 编排（start_debug → wait_for_event → 后续）。这样保持每个工具单一职责

### K-32：白名单 / 黑名单分散在工具内部、不可观察、有重复定义 ✅ 已修复
- **状态**：2026-05-28，结构重构 + UI 集成
- **现象**：
  1. 用户问"我怎么给 run_dbg_command 加白名单"找不到地方查；
  2. K-30 的 `kSysMods` / `kHotApis` / `classifyBpAddr` 在 `debug_write_tools.cpp` 和 `advanced_bp_tools.cpp` 各有一份**完全相同的副本**，将来加新断点类工具会出现第三份；
  3. 用户在 "已注册工具一览" 看不到 run_dbg_command 实际允许什么命令，也不知道 set_breakpoint 拒绝什么 API
- **修复**：
  - **重构**：新建 `src/ai/tools/bp_safety.{h,cpp}` 集中持有：
    - `bpSysModules()` / `bpHotApis()` / `isSystemModule()` / `isHighFreqApi()` / `classifyBpAddr()` / `HotSpotInfo`
    - `dbgCmdWhitelistDefaults()`（13 项默认硬集）/ `dbgCmdWhitelist()`（默认 + Config::extraDbgCmdWhitelist 合并集）
  - `debug_write_tools.cpp` 和 `advanced_bp_tools.cpp` 删除本地副本，改 include bp_safety.h
  - **UI 集成**：新建 `src/ui/safety_browser_dialog.{h,cpp}`，4 tab 视图：
    1. **白名单** tab：表格列默认 / 用户追加双来源标签，带搜索 + 计数 + 双击复制
    2. **系统模块黑名单** tab：24 个系统 DLL 名（无扩展、小写），带搜索
    3. **高频 API 黑名单** tab：~60 个 API 符号名，带搜索
    4. **如何配置** tab：完整 config.json 示例 + 一键打开/复制路径，提示"修改后必须重启 x64dbg"
  - **交叉引用**：
    - `tools_browser_dialog`（已注册工具一览）底部加 **「安全护栏…」** 按钮 → 打开 SafetyBrowserDialog
    - 三个受影响的工具（`run_dbg_command` / `set_breakpoint` / `set_hw_breakpoint`）的详情窗里加 **「查看此工具的安全护栏…」** 按钮，自动跳到相关 tab
- **设计取舍**：
  - 白名单**只能追加不能从默认集移除**（在 SafetyBrowserDialog Tab1 显式说明）；要禁用默认命令必须改代码，避免用户/LLM 通过编辑配置绕开安全护栏
  - `dbgCmdWhitelist()` 合并集是 `static once_flag`，**修改 config 必须重启插件**才生效；这是有意为之（避免运行时白名单漂移导致 audit 不一致）
  - 黑名单（K-30 sys/hot）目前**不开放配置追加**——它们是"防卡死"的硬护栏，加错一条用户可能瞬间冻结桌面。未来若有合理用例可加 `extra_safe_apis`（白名单覆盖黑名单）字段
- **影响**：
  - 用户在 UI 上能完整看到所有 ACL 规则的具体内容
  - 工具开发者：未来加第三个断点类工具直接 `#include "ai/tools/bp_safety.h"` 即可，无需复制粘贴
- **位置**：
  - `src/ai/tools/bp_safety.{h,cpp}` 新增
  - `src/ai/tools/debug_write_tools.cpp:99-115` 重构后只剩注释说明（原 95-235 行删）
  - `src/ai/tools/advanced_bp_tools.cpp:88-90` 同上
  - `src/ui/safety_browser_dialog.{h,cpp}` 新增
  - `src/ui/tools_browser_dialog.cpp::buildUi` + `showToolDetails` 加按钮
  - `src/CMakeLists.txt:57-59` + `129-131` 加新文件

---


## ⚪ 未支持（设计取舍，不是 bug）

### N-01：Linux ELF / 跨平台
- 插件只对 x64dbg 的 Windows PE 目标设计；ELF 不支持
- 原因：调用栈采样依赖 RtlVirtualUnwind / SEH 信息，逻辑与 PE 紧耦合

### N-02：32 位 trace 的 EBP/ESP 假设
- 32 位下假设标准 ebp 链 + cdecl/stdcall；优化掉 frame pointer 的代码采栈可能不准
- 已知问题：x86 release 模式 + /Oy 编译的代码栈帧可能缺失

### N-03：Provider 切换不持久化历史会话的 provider
- 会话只记 model 名，没记 provider
- 切到 DeepSeek 后打开一个旧的 Copilot 会话，再发消息会用当前 provider 发——可能错配模型
- 缓解：modelBox 切换时会自动按当前 provider 重新拉 model 列表；如果用户没主动切回原 provider 容易踩

### N-04：导入会话不携带 RAG chunks
- M3.6 故意为之：避免污染当前项目 RAG 上下文
- 副作用：导入的对话历史无法在新项目里被语义检索命中
- 用户可在导入后手动重做关键反汇编 → 自动写新 chunks

### N-05：全局分析强制 token 预算确认
- 任何"分析整个模块 / 全部函数"类操作都要求用户先确认估算的 token 数
- 设计取舍：防止误触烧光额度

### N-06：sub-agent / 子工作流框架不支持（G-2 / S9 后续 决策）
- 当前 AgentLoop 单上下文单 LLM 会话；不支持父 agent 调子 agent
- 原因：方案 C 期间评估，子 agent 收益主要是上下文隔离，但会破坏 prompt cache 共享 + 双倍 system prompt 成本；inline PHASE 0 已能解决"预设前提自检"诉求
- 后果：若未来想做"先 sample-triage 自动选预设再跑"这种链式自动化，需引入 sub-agent 机制（非小改）
- 记于 `decisions.md`，重审触发条件：用户提出明确多预设链式跑诉求

---

## 📋 维护检查清单

定期跑（建议每次发版前）：

- [ ] `projects/*.db` 里 0 KB 文件清理
- [ ] `logs/plugin.log` 大小检查（spdlog 默认 4 MB rotate 5 份）
- [ ] DPAPI 凭据文件是否仍能解密（用户机器换 Windows 账户会失效）
- [ ] vcpkg baseline 更新检查（`vcpkg upgrade --no-dry-run`）
- [ ] 双架构 .dp32/.dp64 都能在最新 x64dbg snapshot 加载
- [ ] trace_demo Targeted Trace 回归（应 RETURNED）
- [ ] CallStack 采样回归（JX3ClientX64 send 应 32 hits / 2 unique）
- [ ] G-2 cache 观测：sample-triage 跑两轮，第二轮顶栏应显示 `cache=`>50%（DeepSeek）
- [ ] 加壳样本（如 `cs_fuc_call_se.exe`）跑 unpack-helper：PHASE 0 应在 ≤3 calls 内识别 packer signature 进入 PHASE 1
- [ ] 未加壳样本跑 unpack-helper：PHASE 0 应在 ≤3 calls 内 STOP 并建议改 sample-triage / analyze-function
