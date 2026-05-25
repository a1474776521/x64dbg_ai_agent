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
