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

### K-33：5s confirm 弹窗不可豁免 + 允许按钮无快捷键 ✅ 已修复
- **状态**：2026-05-28
- **现象**：
  1. 用户在 agent 自动跑 30 轮的 task 里被弹 20+ 次 `set_label` / `set_comment` / `add_function` 这种"几乎没风险"的写工具确认窗，每次都要等 5s 才能点；要么全程盯着键鼠，要么干脆豁出去全允许
  2. 倒计时结束后想快速允许只能鼠标点按钮；ESC/Enter 又被绑成"拒绝"（安全默认），无法用键盘加速允许
- **修复**：
  - **配置驱动豁免**：`AppConfig::autoApproveTools` 从 `config.json` 的 `auto_approve_tools: [...]` 加载（小写化、长度 sanity）；启动时 `std::call_once` 合并入 `confirm_policy.h` 暴露的 `autoApproveTools()` 集
  - **新建 `src/ai/tools/confirm_policy.{h,cpp}`**：暴露
    - `confirmHardEnforced()` / `isConfirmHardEnforced(name)` — 5 项硬黑名单（`run_dbg_command` / `start_debug` / `attach_debug` / `stop_debug` / `patch_file`）
    - `autoApproveTools()` / `isAutoApproved(name)` — 综合判定（先黑名单过滤再查配置）
  - **`tool_registry::dispatch` 集成**：`needConfirm = isWrite && requiresUserConfirmation() && !isAutoApproved(name)`；豁免命中时仍写一条 `phase: "auto_approved"` 的 audit log
  - **`ToolConfirmDialog` 加快捷键**：`Ctrl+Enter` / `Ctrl+Return` 双绑（QShortcut + lambda 显式检查 `allowButton_->isEnabled()`，避免倒计时未到时被触发）；按钮文案从「允许」→「允许 (Ctrl+Enter)」，ToolTip 提示「倒计时结束后生效」
  - **`SafetyBrowserDialog` 加 Tab4**「Confirm 豁免 (K-33)」：列出所有 Write 工具 + 状态标签（"强制 confirm (黑名单)" / "✅ 已豁免 (用户配置)" / "默认弹 5s confirm"）；Tab5「如何在 config.json 配置」补充 `auto_approve_tools` 示例 + 骨架文件创建也加上空字段
- **设计取舍**：
  - **「宽松黑名单」策略**：除 5 项黄金黑名单外，所有 Write 工具都允许用户豁免——给重度用户最大灵活度。理由：黑名单覆盖了"会启动/杀进程 + 会落盘 + 命令逃生口"三类高风险，剩下的（set_breakpoint / patch_memory / assemble_at / set_register / set_page_protect / write_string ...）虽然能改进程状态但都是可观察可撤销的，由用户判断风险阈值
  - **仍写 audit**：豁免 ≠ 不记录。`phase: "auto_approved"` 让事后复盘"我什么时候改过这个 dword"仍可 jq/grep。磁盘开销近零（rotating 4MB×10）
  - **UI 只读 + config 编辑 + 重启生效**：与 K-32 白名单模式一致；用户不能在运行时切换豁免（避免 audit 解释不一致）
  - **`Ctrl+Enter`** 选择理由：业界惯用「危险操作确认」组合键（Telegram / Slack / GitHub PR 都是）；不与 `Esc/Enter=deny` 冲突；倒计时未到时按了也无效（双重保险）
- **影响**：
  - 重度自动化 task 体验大幅改善（豁免 8 项常用低风险写工具后，30 轮 task 平均少弹 15+ 次）
  - 倒计时一到 `Ctrl+Enter` 立即放行，键盘党不再被鼠标拖累
  - 安全护栏不退步：5 项黄金黑名单 + K-30 内容护栏全部保留
- **位置**：
  - `src/ai/tools/confirm_policy.{h,cpp}` 新增
  - `src/util/config.{h,cpp}`：`AppConfig::autoApproveTools` + `auto_approve_tools` 字段解析
  - `src/ai/tools/tool_registry.cpp:164-200`：`needConfirm` 计算 + auto_approved audit
  - `src/ui/tool_confirm_dialog.cpp:48-80,87-96`：QShortcut + 按钮文案/ToolTip
  - `src/ui/safety_browser_dialog.{h,cpp}`：Tab4 新增 + Tab5 示例更新
  - `src/CMakeLists.txt`：加 confirm_policy.{cpp,h}

---

### K-34：`malware-triage` 预设证据链薄弱、无量化、无 ATT&CK 映射 ✅ 已修复
- **状态**：2026-05-28
- **现象**：原版 `malware-triage` 预设虽已有 PHASE 0 verdict gate + 8 步 PHASE 1，但精度不足：
  1. **API 命名启发式弱**：只看 import 表关键字，对动态 `GetProcAddress` 解析的样本完全瞎；良性程序大量误判（浏览器 / IM 都用 WinHttp+CreateMutex）
  2. **零字符串挖掘**：硬编码 C2 URL / 持久化注册表键 / cmd&PowerShell 启动器 / base64 配置块等关键 IOC 没有任何工具能捞
  3. **零 PE 头分析**：异常 TimeDateStamp / RWX 节 / 已知壳 marker（UPX/VMP/Themida）/ entropy 异常 / Authenticode 缺失等"教科书级"恶意特征拿不到
  4. **判定不量化**：输出"该样本疑似恶意"无评分、无置信度、无权重；用户不知道凭什么这么判
  5. **无 ATT&CK 标准化**：自由文本输出，与外部 threat intel 库无法交叉
- **修复（中量级方案）**：
  - **新增 2 个取证工具**（均 Read，纯只读）：
    - `scan_strings(module? | start+size, min_len, encoding=both, only_suspicious)`：内存范围或整个模块映像扫描 ASCII + UTF-16LE 可打印字符串，按启发式分类标签 `c2_url` / `c2_ip` / `c2_onion` / `cmd_exec` / `registry_persist` / `path_env` / `mutex_marker` / `crypto` / `base64_blob`；硬上限 64 MB / 2000 条；分块 1MB 读 + 坏页降级到 4KB 探测；命中页 / 坏页计数附在结果里
    - `analyze_pe_header(module?)`：用 vcpkg `pe-parse 2.1.1` 解析磁盘上的 PE 文件（image mapped 后 raw section 数据不可靠），输出 machine / subsystem / TimeDateStamp（含未来时间/epoch=0/>20 年异常）/ Entry Point（含 `ep_in_last_section` 检测）/ Image CheckSum / DLL Characteristics（NX/ASLR/CFG/HVCI/...）/ 节表（每节 Shannon entropy + RWX + 已知壳 marker 16 种：UPX/ASPack/VMProtect/Themida/Enigma/PECompact/MPRESS/Petite/NsPack/y0da/BOOM/MEW...）/ 资源表类型直方图（>100 KB 单条标 oversized）/ Data Directory 关键 6 项（Import/Export/Cert/Reloc/Debug/TLS）/ Authenticode 存在性（不验链）；输出量化 `risk_score 0-100` + `risk_tags` 数组
  - **`malware-triage` system prompt 重写为 PHASE 0/1/2/3 四阶段**：
    - PHASE 0：`analyze_pe_header` 一发判加壳 / 加密 / EP 在末节
    - PHASE 1：`analyze_pe_header` + `get_module_imports`（按 INJECTION/C2/PERSIST/CRYPTO/AV-EVASION 五类聚合）+ `scan_strings only_suspicious=true`
    - PHASE 2：原有行为面（PEB/线程/句柄/TCP/窗口/SEH）
    - PHASE 3：**量化评分 rubric**——明确给 PE risk_score / 注入 import / C2 import / c2_url / cmd_exec / registry_persist / mutex_marker / HideFromDebugger / 活跃 C2 连接 / RWX 私有区每项的权重，封顶 100；分四档（0-19 benign / 20-44 suspicious / 45-69 likely-malicious / 70-100 highly-likely-malicious）
  - **强制 MITRE ATT&CK 映射**：injection-imports + RWX → T1055 + 子技术；C2-imports → T1071；registry_persist → T1547.001；CreateService → T1543.003；反调试 → T1622；cmd/powershell → T1059.001/003；CryptEncrypt + FindFirstFile → T1486
  - **输出格式标准化**：四段（结论 / 关键 IOC / 行为画像 / ATT&CK 矩阵 / 建议后续动作），每个 IOC 必须 cite tool.field 来源 + 权重 + ATT&CK ID
  - **`maxIter` 25 → 30**：新增的两步静态深挖（PE + strings）让循环预算合理
- **设计取舍**：
  - **未引入 LIEF**：LIEF 体积 ~10MB + 编译 15-20 min + 一堆传递依赖；pe-parse 体积 500KB + 编译 30s 已能覆盖核心字段。Authenticode 仅检"存在性"——足以作为风险信号，链验证留给外部 sigcheck.exe / signtool
  - **未引入 YARA**：方案锁定中量级，规则集维护成本与本项目"工具增强"定位不符；后续如有需求可单独开 K-37 集成 libyara
  - **未引入动态采样**：保持取证预设严格只读 = 不污染样本（符合数字取证规范），动态行为采集职责划给 `unpack-helper` 等其他预设
  - **PE 头从磁盘读而非内存**：image mapped 后 SizeOfRawData 不可信，entropy / Authenticode 全乱；磁盘读 = 拿到原始可靠数据，符合「样本指纹」需求
- **影响**：
  - 恶意代码取证 agent 输出从"自由叙述"升级到"量化评分 + 证据链 + ATT&CK 矩阵"三件套
  - 工具总数 74 → **76**
  - dp64 体积 11.79 MB → **14.53 MB**（+2.74 MB，pe-parse 静态链接代价）
- **位置**：
  - `src/ai/tools/scan_strings_tool.cpp` 新增 ~370 行
  - `src/ai/tools/analyze_pe_header_tool.cpp` 新增 ~450 行（**注意 `<pe-parse/parse.h>` 必须在 `<Windows.h>` 前 include，否则 `IMAGE_SUBSYSTEM_*` 等同名宏冲突触发 C2059**）
  - `src/ai/tools/builtin_tools.h`：暴露 `registerScanStringsTool` / `registerAnalyzePeHeaderTool`
  - `src/ai/tools/tool_registry.cpp:89-92`：注册
  - `src/ai/agent_preset.cpp:598-670`：malware-triage 重构
  - `src/CMakeLists.txt`：加 2 个新源 + link `pe-parse::pe-parse`
  - `CMakeLists.txt` 顶层：`find_package(pe-parse CONFIG REQUIRED)`
  - `vcpkg.json`：加 `"pe-parse"` 依赖

---

### K-35：AgentLoop 单循环缺自愈与历史召回（tool retry + auto-RAG 注入） ✅ 已修复
- **状态**：2026-05-29
- **现象**（来自 `docs/agent-capability-assessment.md` 评估，agent 处 L2 末/L3 初）：
  1. **无 tool retry**：`registry.dispatch` 一旦失败（ok=false），即便是 embedding 接口抖动 / HTTP 5xx / 连接超时这类瞬时错误，也直接把错误文案回吐给 LLM，浪费一整轮往返让模型"自己重试"，且模型未必会重试
  2. **RAG 不自动注入**：历史分析虽已写入 SessionStore 的向量库，但 LLM 必须显式调 `rag_search` 才能召回；实测模型经常不调，导致跨轮 / 跨会话的已有结论被白白浪费（与 N-04 同源）
- **修复（K-35 编排增强，两项小而稳）**：
  - **tool retry**（`agent_loop.cpp` dispatch 处包一层）：
    - 仅对**瞬时错误**重试 —— 错误文案命中白名单关键字（`timeout`/`connection`/`network`/`embedding failed`/`rate limit`/`503`/`502`/`504`/`500`/`service unavailable`/`reset by peer`/`broken pipe`/`ssl` 等），见 `isTransientToolError()`
    - 仅对**非 Write 类**工具重试（`registry.categoryOf(name) != ToolCategory::Write`）：Write 已确认的副作用（断点/dbg cmd/patch）绝不能重复触发
    - 退避：`300ms * attempt`（300/600/900）；每次重试前后查 `cancel`；若中途错误转为非瞬时则立即停止
    - 默认开、最多 1 次（`tool_retry_enabled=true` / `tool_retry_max=1`，上限 3）
  - **auto-RAG 注入**（`agent_loop.cpp` run 入口，仅注入一次）：
    - 取首条 user 消息（>4000 字符截断）→ `EmbeddingClient::embed` → `SessionStore::searchSimilar(topK)` → 拼成一条 `system` 消息插到**最后一条 user 消息之前**
    - 单条 chunk 文本 >1200 字符截断；明确标注"背景信息，依赖前需用 live 工具核实"
    - 复用 `dynamic_context_tools.cpp` 里 `rag_search` 已验证的检索路径（同 embed + 同 searchSimilar）
    - 无 SessionStore / embed 失败 / 无结果 → 静默跳过（不阻断 run）
    - 默认开、top-K=4（`auto_rag_inject_enabled=true` / `auto_rag_top_k=4`，1-16）
- **设计取舍**：
  - retry 放 AgentLoop 层而非 dispatch 层：loop 能拿到 `categoryOf` + `cancel`，且不必污染 `ToolResult` 结构 / dispatch 签名
  - 瞬时错误用**文案白名单**而非给 `ToolResult` 加 `retryable` 标志：零侵入，无需改 70+ 个工具
  - auto-RAG 只在 run 入口注入一次（不是每轮）：避免重复 embedding 烧 GitHub Models 配额 + 重复刷 context；后续深挖仍靠 LLM 显式 `rag_search`
  - **本次两个开关默认开**（用户明确选择"都默认开"）—— 与以往"默认关、config 显式开"的偏好不同；retry 已用"仅瞬时 + 仅非 Write"严格收口避免掩盖真实错误
- **影响**：
  - 缓解 **N-04**（导入/历史会话 RAG 召回）：首条问句相关的历史 chunks 现在会被自动注入；但仍只覆盖"首轮 + 当前 session store"
  - 瞬时网络/接口抖动不再消耗整轮 LLM 往返
  - 工具数 / 体积不变（纯 AgentLoop + config 逻辑改动，无新依赖）
- **位置**：
  - `src/ai/agent_loop.cpp`：`isTransientToolError()` / `buildAutoRagContext()` + run 入口注入 + dispatch retry 循环
  - `src/util/config.{h,cpp}`：`toolRetryEnabled` / `toolRetryMax` / `autoRagInjectEnabled` / `autoRagTopK`（config.json 键 `tool_retry_enabled` / `tool_retry_max` / `auto_rag_inject_enabled` / `auto_rag_top_k`）

---

### K-36：AgentLoop 工具串行执行 + 上下文无界增长（并行 read + 上下文压缩） ✅ 已修复
- **状态**：2026-05-29
- **现象**（接 `docs/agent-capability-assessment.md` 评估第 4/3 项）：
  1. **工具严格串行**：一轮 LLM 回吐 N 个 tool_calls 时，`agent_loop.cpp` 用串行 `for` 逐个 dispatch。多个**只读**探查（read_memory/disasm/list_xrefs…）本可并发却被串成一条线，慢
     - 注：UI 早就把一轮多卡片一次性预创建成 pending（`assistant_panel.cpp:1188`），**视觉上像并行**，但底层一直是串行 dispatch —— 这是用户反馈"看起来已经并行 read"的来源
  2. **上下文无界增长**：`req.messages` 每轮只增不减，长会话（多轮工具往返）累积到撞 provider 上下文窗口直接 400 / 截断
- **修复（K-36 编排增强）**：
  - **并行 read**（`agent_loop.cpp` 工具执行块重构）：
    - 把 dispatch+retry 抽成 `runOneTool(tc)` lambda，串行/并行两路复用（retry 逻辑 K-35 完全保留）
    - **仅当本批 tool_calls 全为 Read 类 + 开关开 + count>1** 时，用 `QThreadPool` + `QtConcurrent::run` 并发（最多 `parallelReadMax` 个，默认 4）
    - **只要有一个 DbgControl/Write 立即回退串行**：保证 confirm 弹窗顺序 + audit 顺序 + 写副作用时序不被打乱
    - 结果用 `std::vector results[idx]` 按**原始下标**收集；回调（`onToolReport`）+ append tool 消息**始终在主循环线程按原序做**（cb 最终发 Qt signal，非线程安全；且 tool 消息顺序必须 == tool_calls 顺序才能配对）
    - 默认开、上限 4（`parallel_read_enabled` / `parallel_read_max`，1=关、上限 8）
  - **上下文压缩**（`agent_loop.cpp` 每轮 streamChat 前）：
    - token 粗估 `estimateTokens`：字符数/4 + 每条 4 token 结构开销（无 tokenizer）
    - 模型窗口 `providerContextWindow(model)`：deepseek 64K / gpt-4o·4.1·o-series 128K / claude 200K / 未知保守 32K
    - 超过 `窗口 * contextCompressThresholdPct%`（默认 75%）→ 反复折叠**最老整轮**直到达标
    - **整轮折叠**（`compressOldestRound`）：一条 assistant(带tool_calls) + 其后紧跟的全部 tool 消息作为一个原子单元，折叠成一条 system 摘要（本地拼 role+工具名+前 300 字符，**不调 LLM**）
    - **永不压缩**：开头连续 system（含 auto-RAG 注入）+ 末尾 `contextCompressKeepRounds` 轮（默认 3）+ 末轮
    - 默认开、阈值 75%、保留 3 轮（`context_compress_enabled` / `context_compress_threshold_pct` / `context_compress_keep_rounds`）
- **设计取舍 / 关键正确性点**：
  - **整轮折叠是配对正确性的命门**：OpenAI/DeepSeek 协议要求 assistant 的每个 tool_call 后必须紧跟同 `tool_call_id` 的 tool 消息；若按"消息条数"截断会把配对拆散触发 provider 400。按"轮单元"折叠保证 assistant↔tool 永远成套删/留
  - **并行只并行 dispatch**：回调和 messages.push_back 留主线程串行，规避 cb 线程安全 + 顺序问题
  - **lambda 捕获坑**：并行任务捕获 `idx`（值）而非 `&tc`（引用循环变量会因循环推进而全部指向最后一个，悬空）；lambda 内用 `st.toolCalls[idx]` 取
  - **并行后端选 QtConcurrent**：agent_worker.cpp 同目录已用、`src/CMakeLists.txt` 已链 `Qt5::Concurrent`，零额外依赖
  - **本地截断而非 LLM 摘要**：压缩零额外 LLM 成本/延迟；代价是摘要质量低（仅前 300 字符），但折叠的是"最老"轮，近期上下文完整保留
  - **x64dbg 读 API 多线程安全性**：`tool_registry.h` 注明"dispatch 可并发、工具自保证线程安全"；并行 read 依赖此前提，需真机小样本验证不崩（见验收清单）
- **影响**：
  - 多只读探查从串行压成并发（上限 4），明显提速
  - 长会话不再撞窗口 400；超阈值自动折叠最老轮
  - 缓解 **N-05**（token 预算）：现在有自动压缩兜底，不必每次靠用户确认 token
  - 工具数 / 体积基本不变（纯 AgentLoop + config 逻辑）；dp64 11.65MB / dp32 8.57MB
- **位置**：
  - `src/ai/agent_loop.cpp`：`estimateTokens` / `providerContextWindow` / `compressOldestRound` + run 入口压缩检查 + 工具执行块 `runOneTool`/并行分支
  - `src/util/config.{h,cpp}`：`parallelReadEnabled` / `parallelReadMax` / `contextCompressEnabled` / `contextCompressThresholdPct` / `contextCompressKeepRounds`

---

### K-37：K-35 retry 误判调试器业务超时 + confirm 倒计时偏长 + run_continue 上限过严 ✅ 已修复
- **状态**：2026-05-29
- **现象**（来自 2026-05-29 10:xx 用户真机会话 `unpack-helper` 跑 UPX 样本的 plugin.log 复盘）：
  1. **K-35 retry 误判**：3 次 `wait_for_event` 业务 timeout（没等到断点）被 `isTransientToolError` 的 `"timeout"` 关键字命中当作"瞬时错误"自动重试，每次重试又等同样长 → iter#7 浪费 30s、iter#14 / iter#26 各浪费 60s、共浪费 ~150 秒。其它 `run_continue timeout` 因 category=Write 已被 K-35 排除未被误重试
  2. **confirm 倒计时太长**：用户反馈 5s 倒计时过长，UPX 脱壳要按多次"允许"很烦躁
  3. **`run_continue` `timeout_ms` 上限 60s 过严**：LLM 试图传 `timeout_ms=120000` 被 `tryGetInt32Hint(args, "timeout_ms", 100, 60000, ...)` 拒（iter#30 `invalid 'timeout_ms': value out of range [100, 60000]`）。unpack/trace 场景下 60s 经常不够，应放宽到 5 分钟
- **修复**：
  - **K-37.1 retry 排除 DbgControl 类**（`agent_loop.cpp` 重试条件）：
    - 原 K-35 条件：`categoryOf != Write`
    - 新条件：`categoryOf != Write && categoryOf != DbgControl`
    - DbgControl 类工具（`wait_for_event` / `step_in/step_over/step_out` / `run_until`）的 timeout 几乎都是**业务超时**（没等到目标事件），重试只会再等一次相同的 timeout，浪费时间。`tool_registry.h` 已把这类工具单独分档，本次直接复用
    - 非 DbgControl 的 Read 工具（embedding/HTTP/rag_search 等）仍按原 K-35 规则重试
  - **K-37.2 confirm 倒计时 5s → 3s**：
    - `agent_worker.cpp:63` 硬编码 `countdownSec=5` → `3`
    - `tool_confirm_dialog.h` 构造函数和 `confirmFromBackground` 两处 `int countdownSec = 5` 默认值同步改 3
    - 头部文档注释同步
  - **K-37.3 `run_continue` `timeout_ms` 上限 60000 → 300000**：
    - `debug_navigation_tools.cpp:104` `tryGetInt32Hint(args, "timeout_ms", 100, 60000, ...)` → `300000`
    - `parametersSchema.timeout_ms.description` 同步 `"max 60000"` → `"max 300000 (5 min, raised in K-37 for unpack/trace scenarios)"`
    - `descriptionZh()` 同步 "超时默认 30 秒" → "超时默认 30 秒、上限 300 秒"
    - **未改 `pause_debug`/`step_*`/`wait_for_event` 等其它工具的上限**：那些工具的语义是"短期等待"，300s 上限会让 cancel 响应变慢；K-37 仅放宽 `run_continue`（unpack/trace 主力工具）
- **设计取舍**：
  - **为什么不在 `isTransientToolError` 加错误文案反白名单（如排除 "waiting for next stop"）**：白名单/反白名单都是字符串启发式，新工具/新错误文案要持续维护。按 `ToolCategory` 整段排除更稳：DbgControl 这一档本就是"等事件类"，其失败语义先天与"网络抖动可重试"对立
  - **为什么 `run_continue` 的 category 是 Write 不是 DbgControl**：历史遗留——`run_continue` 改了执行流但本质是"继续执行"指令，作者当年归到 Write 是为了走 confirm。K-37 不动这个分类（动了会绕过 5s confirm），仅放宽 timeout 上限解决具体问题
  - **`pause_debug` 用了 `{"string":"true"}` 这种乱七八糟参数**：LLM 错调用，本次不修工具侧（错就该报错让 LLM 学）；可后续在 description 加示例参数引导
- **影响**：
  - 脱壳/trace 场景不再因 retry 误判额外等 1-2 分钟
  - confirm 总等待时间 5s→3s，多步操作累计提速明显
  - `run_continue` 长时运行（如等 OEP）不再因协议上限被卡
  - 工具数 / 体积不变；dp64 11.66MB / dp32 8.57MB
- **位置**：
  - `src/ai/agent_loop.cpp`：retry 条件加 `!= ToolCategory::DbgControl`
  - `src/ai/agent_worker.cpp:63`：`countdownSec=5` → `3`
  - `src/ui/tool_confirm_dialog.h`：两处默认参数 5→3 + 头注释
  - `src/ai/tools/debug_navigation_tools.cpp`：`run_continue` 的 `timeout_ms` 上限 60000→300000 + description 同步

---

### K-38：K-36 上下文压缩按字节截断切坏 UTF-8 多字节序列，provider dump 抛 type_error.316 ✅ 已修复
- **状态**：2026-05-29
- **现象**：用户在 jx3clientx64（剑网三客户端，大量中文 PE 字符串）会话里跑反外挂相关 IOC 检测，AgentLoop iter#3 抛错：
  `provider threw: [json.exception.type_error.316] invalid UTF-8 byte at index 459: 0x2E`
  整个 agent run 中断，无法继续。
- **触发时序**（来自 2026-05-29 11:47:57 plugin.log）：
  1. iter#1 三个 `search_pattern` 并行
  2. iter#2 三个 `scan_strings` 并行（每个返回 2000 条字符串）
  3. iter#3 触发 K-36 上下文压缩 `est_tokens=88817 budget=48000`，折叠 3 轮为一条 system 摘要
  4. 同一 iter#3 立刻 `provider error: [json.exception.type_error.316] invalid UTF-8 byte at index 459: 0x2E`
- **根因**：`agent_loop.cpp::compressOldestRound` 第 220 行
  ```cpp
  if (body.size() > 300) body = body.substr(0, 300) + "...";
  ```
  `body` 来自 `m.content`，含 UTF-8 中文/emoji 多字节字符。`substr(0, 300)` 按**字节**截断会切断 UTF-8 序列：例如 298 字节是 `E5`、299 是 `A4`（"复"的前 2 字节），300 应该是 `8D` 但被切掉，拼上 `"..."` = `E5 A4 2E 2E 2E`。`nlohmann::json::dump()` 验证 UTF-8 时遇到 `E5 A4` 后期待续接字节（`10xxxxxx`），实际遇到 `0x2E`（句点首位为 0），抛 type_error.316。**0x2E 不是非法字节本身，而是"应该是续接字节但不是"的那个字节**。
- **修复**：
  - **K-38.1 `safeUtf8Truncate(s, maxBytes)`**：按 UTF-8 字符边界截断。从 maxBytes 处往前回退到首字节，然后判断该字符的预期续接字节数是否齐了：齐了完整保留、不齐整个砍掉。回退最多 3 字节（UTF-8 最长 4 字节）。`compressOldestRound` 改用 `safeUtf8Truncate(body, 300)`
  - **K-38.2 `sanitizeUtf8(s)` 防御层**：在 `AgentLoop::run` 把 `req.messages` 拷贝给 `creq.messages` 后、`provider->streamChat()` 前，对所有 `message.content` 做一遍非法 UTF-8 替换为 `'?'`。即使将来其他路径（工具结果含 GBK 字节、用户粘贴非法字节、其它截断 bug）泄漏非法字节，dump() 前最后一道关也能挡住，避免整个 agent run 被 provider throw 炸掉。
- **设计取舍**：
  - **为什么不只在两个 provider 客户端 (`copilot_chat_client.cpp` / `deepseek_chat_client.cpp`) 各自加一遍 sanitize**：放在 `agent_loop.cpp` 一处统一更不易漏；新增 provider 也自动受益
  - **为什么 sanitize 替换为 `?` 不是丢弃**：保留字节位置便于 debug；用户在 UI 也能直观看到"哪里有奇怪字节"
  - **为什么不修 `scan_strings`/工具结果**：本次定位的 PE 字符串走 `isAsciiPrintable` 过滤（0x20-0x7E），原则上不会输出非 ASCII；污染源唯一确认在压缩逻辑。若将来发现工具结果泄漏非法字节，sanitizeUtf8 也已兜住
  - **为什么不收紧 `scan_strings` max_items**：当前 6000 条/轮触发压缩是合理使用，问题不是数据多而是压缩 bug
- **影响**：
  - 中文 / emoji / 任何含多字节 UTF-8 字符的长会话不再被压缩 bug 炸掉
  - 即便将来引入新的非法字节路径，provider 也不会 throw
  - 工具数 / 体积不变；dp64 11.66MB / dp32 8.57MB
- **位置**：
  - `src/ai/agent_loop.cpp`：新增 `safeUtf8Truncate` / `sanitizeUtf8` 两个 inline 函数；`compressOldestRound` 改用 safe 截断；`run()` 在 `creq.messages` 赋值后加 sanitize 循环
  - **K-39 衍生重构**：两个 helper 抽到 `src/util/utf8_safe.h` namespace `x64ai::util`，agent_loop.cpp 改 include + `using`；K-39 新加的 `shell_cmd`/`shell_pwsh` 解码 stdout 时复用同两个 helper（cmd 用 `GetACP()` 解 GBK 后 sanitize 兜底；pwsh 注入 UTF-8 输出后仍 sanitize 防截断）

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

### N-04：导入会话不携带 RAG chunks（K-35 部分缓解）
- M3.6 故意为之：避免污染当前项目 RAG 上下文
- 副作用：导入的对话历史无法在新项目里被语义检索命中
- 用户可在导入后手动重做关键反汇编 → 自动写新 chunks
- **K-35 缓解**：auto-RAG 注入会在每次 run 入口按首条问句自动召回当前 session store 里的 chunks（默认开）；但仅"首轮 + 当前 store"，跨项目导入的历史仍不在范围内

### N-05：全局分析强制 token 预算确认（K-36 部分缓解）
- 任何"分析整个模块 / 全部函数"类操作都要求用户先确认估算的 token 数
- 设计取舍：防止误触烧光额度
- **K-36 缓解**：AgentLoop 现有上下文压缩兜底（超模型窗口 75% 自动折叠最老整轮），长会话不再硬撞窗口；但这是 agent 运行时的被动压缩，N-05 的"主动全局分析预算确认"仍保留

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
