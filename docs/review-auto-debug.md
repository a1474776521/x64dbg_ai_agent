# x64dbg AI 插件 — 自动化调试方向全量审查报告

> 审查范围：自动化调试 + AI 协助自动化调试。
> 审查模式：先报告、不动代码；落地项等用户挑选。
> 证据格式：所有结论附 `file:line`，可直接跳转复核。
> 严重度：**Critical** = 必修；**High** = 强烈建议修；**Medium** = 计划内修；**Low** = 改进。

---

## 0. TL;DR

- **2 个 Critical Bug 已坐实**：
  - C-1：`CB_STOPDEBUG` 被双注册，`plugin_callbacks::cbStopDebug` 被 `TraceRecorder` 覆盖 → `ProjectContext::onDebugStop` / `AssistantPanel::onDebugStopped` **永不触发**。
  - C-2：`cbInitDebug` 在调试线程同步算 SHA256 + 打开 sqlite，大体积 exe 会卡 UI 几秒。
- **1 条 High Bug**：`read_memory` 入参类型校验过严，LLM 传字符串化数字直接失败到 maxIter。
- **自动化调试的核心缺口**：当前 12 个工具几乎**全部只读**，缺写操作（断点/单步/写内存/写寄存器）+ 缺事件驱动（断点命中 → 唤起预设）+ 缺同步原语（`wait_for_event`）。Agent 不能"驱动"调试器，只能"旁观"调试器。
- **首要落地建议**：先修 C-1/C-2/B1（半天工作量），再按本报告 §5 顺序补 P0 写工具与事件 hook。

---

## 1. Critical Bugs（必修，全部带证据）

### C-1 [Trace/Plugin] `CB_STOPDEBUG` 双注册导致 ProjectContext.onDebugStop 丢失 ✅ 已修 (2026-05-24)

> 落地：`plugin_callbacks.cpp::cbStopDebug` 改为单点分发，依次调用
> `AssistantPanel::onDebugStopped` → `TraceRecorder::onStopDebug` →
> `CallStackTracer::onStopDebug` → `ProjectContext::onDebugStop`。
> `trace_recorder.cpp` 已移除 `CB_STOPDEBUG` 的 register/unregister，
> 同时删去内部静态 `cbStopDebug` 函数。详 development-log §S0-C1。

**证据**

- `src/plugin/plugin_callbacks.cpp:49-50`
  ```
  _plugin_registercallback(pluginHandle, CB_STOPDEBUG,
                           reinterpret_cast<CBPLUGIN>(cbStopDebug));
  ```
  其中 `cbStopDebug` 内部调用 `AssistantPanel::onDebugStopped()` + `ProjectContext::instance().onDebugStop()`（`plugin_callbacks.cpp:30-34`）。
- `src/trace/trace_recorder.cpp:75-76`
  ```
  _plugin_registercallback(pluginHandle, CB_STOPDEBUG,
                           reinterpret_cast<CBPLUGIN>(cbStopDebug));
  ```
- `src/plugin/plugin_callbacks.cpp:55` 显式在 `plugin_callbacks::registerCallbacks` 末尾调用 `TraceRecorder::instance().registerCallbacks(pluginHandle)`，即 trace 的 `CB_STOPDEBUG` **后注册**。
- 行为：x64dbg SDK `_plugin_registercallback` 同 (plugin, type) 后注册替换前注册，因此 **plugin_callbacks 的 cbStopDebug 被静默覆盖**。

**后果**

- ProjectContext 不会落盘当前会话总结；二次开调试时 SHA256 / sessionId 状态仍是上一程序。
- AssistantPanel 调试态 UI 不复位（按钮、占位符），M4 Agent 在"调试停止"后仍认为 `debuggerActive`，进而读到野指针。

**修复方案**

- 将 trace 的回调迁移到 `TraceRecorder::onDebugStop()` 公开方法，由 `plugin_callbacks::cbStopDebug` 统一分发；trace_recorder 不再单独注册 `CB_STOPDEBUG`。
- 备选：保持双方各自的回调，但在 plugin_callbacks 中把 `cbStopDebug` 改为 thunk，先转发到 trace，再做其余清理；此方案需手动保证注册顺序。

---

### C-2 [Plugin] `cbInitDebug` 主线程同步 SHA256 + sqlite，阻塞调试器主线程 ✅ 已修 (2026-05-24)

> 落地：`ProjectContext::onDebugStart` 内部异步化。
> 主线程立即抢占 `generation_++` 并清空 `store_`/`projectId_`；
> SHA256 + sqlite 打开 + meta 写在 detach 线程执行；
> 完成前若被新一轮 `onDebugStart` 或 `onDebugStop` 抢占（generation_ 不匹配）则丢弃。
> 新增 `isIndexing()` 供 UI 显示"指纹化中"占位。
> 消费者读 `store()` 均已具备 nullptr 软返回。详 development-log §S0-C2。

**证据**

- `src/plugin/plugin_callbacks.cpp:` `cbInitDebug` 同步调用 `ProjectContext::instance().onDebugInit(path)`。
- `src/storage/session_store.cpp` + `src/util/hashing.cpp:43-72`：在调用线程顺序读文件 → SHA256 → 打开 sqlite → 建表。
- x64dbg 在 `CB_INITDEBUG` 时仍在调试器主线程（GUI 线程的同步消息）。

**后果**

- 调试几百 MB 的目标程序，启动卡 2–6 秒；调试 1 GB+ dump/壳前样本时观感"x64dbg 卡死"。

**修复方案**

- `ProjectContext::onDebugInit` 改为投递 `QThreadPool` 异步任务，UI 显示"正在指纹化…"占位符；SHA256 完成后 emit 信号回 UI 线程刷新 AssistantPanel 标题与项目库句柄。
- AssistantPanel 在 `projectId` 就绪前禁用 Agent 入口（grey out + tooltip）。

---

## 2. High Bugs（强烈建议修）

### H-1 [Tools] `read_memory` 拒绝字符串化整数，LLM 反复失败到 maxIter ✅ 已修 (2026-05-24)

> 落地：`basic_read_tools.cpp` 新增 `parseInt32Lenient(v, lo, hi, out, err)`，
> 支持 JSON number / 十进制字符串 / `0x...` 十六进制字符串，clamp 到 `[lo, hi]`，
> 错误消息明确范围。`read_memory.size` 改用 `[1, 65536]`；
> `get_disasm.lines` 改用 `[1, 512]`。其他可选 hint 参数（max_frames/limit/top_k 等）
> 走默认值兜底，体验改进留 S1 顺手处理。详 development-log §S0-H1。

**证据**

- `src/ai/tools/basic_read_tools.cpp:241-249`
  ```
  if (!args.contains("size") || !args["size"].is_number_integer()) {
      r.ok = false; r.error = "invalid 'size'";
      return r;
  }
  int size = args["size"].get<int>();
  if (size <= 0 || size > 65536) { ... }
  ```
- 同文件 `get_disasm` 的 `lines` 字段同样只接受 `is_number_integer()`（`basic_read_tools.cpp:155`）。
- 实战日志（fx_log1.txt）：DeepSeek 多次发 `"size":"4096"`，被工具拒后又重发同样字符串，浪费 3–5 个 iteration。

**修复方案**

- 复用 `parseUInt64`（va 已经在用）实现 `parseInt32Lenient`：接受 `number`、`"123"`、`"0x100"`，clamp 到 `[1, 65536]`。
- 错误消息明确告知合法范围，便于模型自纠：`"size must be 1..65536 (decimal or 0x-prefixed hex)"`。

### H-2 [Trace UI] `trace_dialog` 跨线程访问已 destroy 的对话框窗口

**证据**

- `src/ui/trace_dialog.cpp:268-302` worker thread 内通过 raw pointer 回调，未做 `QPointer` 守护。
- `src/ui/trace_dialog.cpp:309-314` close 路径仅 `quit()` worker，不等待 join。

**修复方案**

- worker 用 `QPointer<TraceDialog>` 弱引用；所有 emit 改 QueuedConnection；close 路径加 `wait(2000ms)`。

### H-3 [Locator] `string_scanner` 单字节回退导致 O(n²) 噪声

**证据**

- `src/locator/string_scanner.cpp:165-189`：UTF-16 提取失败时回退为逐字节遍历，未跳过已扫窗口；
- `src/locator/string_scanner.cpp:239-240`：min_len = 1 时退化为读全节。

**修复方案**

- 强制 `min_len >= 4`（UI 侧已默认 4，工具入口未限）；UTF-16 失败时按对齐 2 步进，不退化为 1。

### H-4 [Locator] `pattern_scanner` 节区任意位置匹配单字节 hex

**证据**

- `src/locator/pattern_scanner.cpp:277-283`：pattern 长度 = 1 时未拒绝，扫整节产生数千命中。

**修复方案**

- 校验 `pattern.byteCount >= 3`，否则直接返回 `error="pattern too short"`。

---

## 3. Medium 待复核 / 行为存疑

### M-1 [Provider] `reasoning_content` 回传方向 — ✅ 已实测复核 (2026-05-24)

**当前实现**：`src/ai/deepseek_chat_client.cpp:163-166` 把上一轮 assistant 的 `reasoning_content` 原样回传。

**实测**（`tools/probe_reasoning.cpp`，deepseek-reasoner，两轮多 messages 对比）：

| 组 | 策略 | HTTP | 结论 |
|---|---|---|---|
| A | 回传 reasoning_content | **200** | ✅ |
| B | 剥离 reasoning_content | **200** | ✅ |

**最终结论**：deepseek-reasoner 当前**接受两种形式**。早期"必须回传 否则 400"约束在某时间点被官方放宽。

**处理**：
- 保留 `deepseek_chat_client.cpp:163-166` 回传逻辑（兼容未来协议收紧 + 利于 UI 折叠面板复用）
- 已修正 `chat_provider.h:51-53` 注释口径
- 已同步 docs 多处过时表述（features.md / development-log.md / architecture.md）

**风险解除**：本条不再是技术债。

### M-2 [Agent] reasoning_content 在 agent_loop 累积策略

**证据**：`src/ai/agent_loop.cpp:91-94, 124` 当前把每轮 reasoning 累加到下一轮 system 段，长会话会指数膨胀 token。

**建议**：只保留**最近一轮** reasoning_content，老轮次裁掉。

### M-3 [AssistantPanel] runAgentWithPreset 重入保护不可靠

**证据**：`src/ui/assistant_panel.cpp:991, 1024-1051, 1234` 用 `worker_.isRunning()` 判重；finished 走 QueuedConnection，窗口期内连点产生两个 worker，新 worker 状态被旧 worker tail event 反向覆盖。

**建议**：增加 `std::atomic<bool> agentBusy_`，CAS 抢占；finished slot 内幂等清理。

---

## 4. Auto-Debug Extension Gaps（核心缺口）

> 当前 12 个工具的能力矩阵 — 几乎全是"看"，没有"动"。

| 类别 | 工具数 | 代表 |
|---|---|---|
| 读取反汇编/内存/字符串 | 7 | get_disasm / read_memory / read_string / list_strings |
| 模块/符号/导入 | 3 | list_modules / find_symbol / list_imports |
| Trace 查询 | 2 | trace_summary / trace_stack |
| **写操作** | **0** | — |
| **执行控制** | **0** | — |
| **事件等待** | **0** | — |

### 缺口 G-1 写操作工具缺失

Agent 无法 patch 内存、改寄存器、下断点 → 无法做"自动二分定位 crash"、"绕 anti-debug"、"自动 unhook"。

### 缺口 G-2 执行控制工具缺失

无 step/run/run_to/pause → Agent 无法做"我先单步 20 条看看跳到哪"这种最基本的调试动作。

### 缺口 G-3 事件驱动缺失

`CB_BREAKPOINT` 当前只被 `trace_recorder` 用于录制；没有把"断点命中"转成 Agent 输入。Agent 也没有 `wait_for_event` 让它"挂起等 user 单步完"。

### 缺口 G-4 输出脚本化缺失

无 `run_dbg_command`（即对 `DbgCmdExec` 的受控封装）→ 凡是 SDK 没暴露的能力 Agent 都摸不到。

---

## 5. 新工具建议清单（按优先级）

> 命名沿用现有蛇形小写 + 名词短语风格。Schema 直接给 JSON Schema 草案。所有写工具默认 `requireConfirm=true`（UI 弹确认），可在预设里关掉。

### P0 — 解锁基本自动化调试

#### T-01 `set_breakpoint`
- 入参：`va`（string，必填）, `type`（`"software" | "hardware"`，默认 software）, `condition`（string，可选，x64dbg 表达式）
- 实现：`DbgFunctions()->SetBreakpoint(va, type, condition)` 或 `DbgCmdExecDirect("bp <va>")`。
- 用途：Agent 在分析关键函数前自己下断点。

#### T-02 `delete_breakpoint`
- 入参：`va`（string）
- 实现：`DbgCmdExecDirect("bc <va>")`。

#### T-03 `step`
- 入参：`mode`（`"into" | "over" | "out"`），`count`（int，1..64，默认 1）
- 实现：循环 `DbgCmdExec("sti"|"sto"|"rtr")` + `DbgIsRunning()` 自旋；或 `_dbg_dbgcmdexec`。
- 用途：Agent 自驱动单步采样。

#### T-04 `run_to`
- 入参：`va`（string）, `timeoutMs`（int，默认 5000）
- 实现：临时 bp + run + 等 `CB_BREAKPOINT` 或 timeout。

#### T-05 `pause` / `continue`
- 单参或无参；分别 `DbgCmdExec("pause")` / `DbgCmdExec("run")`。

#### T-06 `wait_for_event`
- 入参：`event`（`"breakpoint" | "pause" | "exception"`），`timeoutMs`（int，默认 30000）
- 实现：内部 condition_variable，被对应的 `CB_BREAKPOINT` / `CB_PAUSEDEBUG` / `CB_EXCEPTION` 唤醒。
- **这是把 Agent 变同步 actor 的关键工具**。

### P1 — 解锁修复/补丁类自动化

#### T-07 `write_memory`
- 入参：`va`（string）, `bytesHex`（string，例 `"90 90"`）, `record_patch`（bool，默认 true）
- 实现：`record_patch=true` → `DbgFunctions()->MemPatch`；否则 `DbgMemWrite`。
- 默认 `requireConfirm=true`。

#### T-08 `set_register`
- 入参：`name`（string，如 `"eax" / "rip"`），`value`（string，支持 hex）
- 实现：`Script::Register::Set*`（third_party/pluginsdk/_scriptapi_register.h）。

#### T-09 `restore_patch`
- 入参：`va`（string，可选；省略 = 全部）
- 实现：`DbgFunctions()->PatchRestore`。

### P2 — 加速 Agent 工作流

#### T-10 `run_dbg_command`
- 入参：`cmd`（string），`allowlist`（内部白名单：`bp/bc/sti/sto/rtr/run/pause/dump/SaveData` 等；写类需 confirm）。
- 实现：`DbgCmdExecDirect`。
- **重要**：必须有白名单否则 Agent 可执行任意 x64dbg 命令，安全风险。

#### T-11 `eval_expression`
- 入参：`expr`（string，x64dbg 表达式）
- 实现：`DbgValFromString`。
- 用途：让 Agent 算 `[rbp+8]+10` 这种偏移。

#### T-12 `list_breakpoints`
- 实现：`DbgGetBpList`。

#### T-13 `get_registers` ✅ 已存在
> `basic_read_tools.cpp:378+` 用 `DbgGetRegDumpEx` 实现，覆盖 32/64 GPR + eflags。
> S1 不再重复实现，但纳入 prompt 文档（让 LLM 知道有这个工具）。

#### T-14 `get_callstack` ✅ 已存在
> `dynamic_context_tools.cpp:77+` 用 `DbgFunctions()->GetCallStack` 实现，
> 含 max_frames 限制 + symbol 解析。同上不重复。

### P3 — 进阶

- `attach_handler`（订阅事件，回调进入 Agent inbox，需要 §6 的事件桥）
- `dump_section`（保存段到文件，便于 Agent 后续静态分析）
- `find_xrefs_to`（基于 trace + scan 的交叉引用聚合）

---

## 6. 架构改造建议

### A-1 事件桥 EventBus

让 x64dbg 回调 → Agent 输入流统一化。

```
+-------------------+   +----------------+   +---------------------+
| x64dbg CB_*       |-->| EventBus       |-->| Agent.wait_for_event|
| (BP/PAUSE/EXC...) |   |  ringbuf+cv    |   | + UI Event panel    |
+-------------------+   +----------------+   +---------------------+
```

- 中心组件 `src/dbg/event_bus.{h,cpp}`（新增）。
- ProjectContext / TraceRecorder / AssistantPanel 全部从 EventBus 订阅，不再各自 `_plugin_registercallback`，**根治 C-1**。
- 单一注册点，便于卸载 / 重载。

### A-2 ToolPolicy 写工具确认机制

- 现有 `ToolContext` 增加 `policy.requireConfirm`、`policy.allowWrite`、`policy.dryRun`。
- AssistantPanel 弹确认对话框（默认 5 秒倒计时取消）。
- 预设 v5 schema 增加 `allowedWriteTools: ["set_breakpoint", ...]` 白名单。

### A-3 AgentLoop 同步原语

- AgentLoop 现仅有 `max_iter` 计数；新增 `pendingEvent` 字段，`wait_for_event` 工具调用时让 loop 真正挂起 worker 线程。
- 唤起后把事件序列化为 `tool_result` 回灌 LLM。

### A-4 持久化分库改异步

- 见 C-2 修复，单独抽 `ProjectIndexer` worker（QThread + QPromise），UI 线程零阻塞。

---

## 7. 工作流草案 — "Agent 真正驱动调试器"

以下三个 workflow 是上述新工具 + 事件桥落地后能直接成立的"杀手级"用例。

### W-1 自动定位首个 CRT 后的用户 main

1. `find_symbol("WinMain"|"main"|"wmain")` 找不到时 → `list_strings(min_len=6)`
2. 选 OEP 后 → `set_breakpoint(oep)` → `run` → `wait_for_event("breakpoint")`
3. 命中后 `get_disasm` + `trace_stack` 反推 CRT 边界
4. 报告："用户 main 在 0x4015A0，CRT 初始化共 12 步"

### W-2 自动 patch 反调试

1. `list_imports("kernel32.dll")` 找 `IsDebuggerPresent` / `CheckRemoteDebuggerPresent`
2. `find_symbol` 拿地址 → `set_breakpoint` → `run` → 命中
3. `write_memory(va_of_xor_eax_eax, "31 C0 C3")` 或直接改 `set_register("eax", "0")` + `step("out")`
4. `continue`

### W-3 自动二分定位 crash

1. `wait_for_event("exception")`
2. 命中后 `get_callstack` + `get_registers` + `read_memory(rip-32, 64)`
3. `set_breakpoint` 到调用方上一帧的 call 指令 → `restart`（需新增 `restart` 工具或走 `run_dbg_command("InitDebug")`）
4. 迭代，每次砍半，直到锁定最小可复现路径

---

## 8. 安全与稳定性硬约束

- **写工具白名单**：`run_dbg_command` 必须 allowlist；非 allowlist 命令一律拒绝并返回错误，绝不"自由放行"。
- **confirm 默认开**：所有 P1 工具（write_memory / set_register / restore_patch）必须 `requireConfirm=true`。
- **超时 / 单次上限**：`step.count <= 64`、`wait_for_event.timeoutMs <= 60_000`、`write_memory.bytesHex` <= 4 KB。
- **dryRun 模式**：预设里可勾选，工具只回报"将要做什么"不真执行，便于 LLM 自测。
- **审计日志**：所有写工具调用强制写 `%APPDATA%\x64dbg-ai-plugin\logs\write_audit.log`，含 sha256(projectId)/va/before/after。

---

## 9. 实施路线建议（用户挑选用）

| 阶段 | 工作量 | 内容 |
|---|---|---|
| **S0 紧急修复** ✅ | 0.5–1 day | C-1 / C-2 / H-1 三条，无新功能（2026-05-24 完成） |
| **S1 读类工具补全** ✅ | 1–2 day | T-11 eval_expression / T-12 list_breakpoints（T-13/T-14 已存在）。预设 schema → v5（2026-05-24 完成） |
| **S2 EventBus + wait_for_event** | 2–3 day | A-1 + T-06，架构改造，trace_recorder 迁移 |
| **S3 P0 写控制工具** | 2–3 day | T-01..T-05 + ToolPolicy 框架（A-2） |
| **S4 P1 修改类工具** | 2 day | T-07/T-08/T-09 + 审计日志 |
| **S5 工作流 demo** | 1–2 day | 三个 W-* 工作流 + 预设 v5 |

总计约 **9–13 工作日** 可拿到一个"Agent 能真正调试"的版本。

---

## 10. 已知待复核项（写在最前等用户拍板）

1. **M-1 reasoning_content 方向**：建议先做一次真机抓包再决定要不要改，本报告不强制结论。
2. **G-4 run_dbg_command 是否引入**：强大但风险高，建议先做 T-11 (`eval_expression`)，run_dbg_command 推迟到 S5。
3. **写工具默认 confirm 倒计时秒数**：5s / 10s / 不倒计时（必须点击），等用户定。
4. **A-1 EventBus 是否替换现注册点**：彻底替换会引发回归测试范围扩大；保守做法是新增 bridge 但保留旧路径，半年后清理。

---

## 11. 附录 A — 调研覆盖清单（7 份 explore agent）

| # | 主题 | 关键文件 |
|---|---|---|
| 1 | agent_loop + 工具体系 | `src/ai/agent_loop.{h,cpp}`, `src/ai/tools/*` |
| 2 | Trace 录制/回放 | `src/trace/*`, `src/ui/trace_dialog.cpp` |
| 3 | Locator（pattern/string） | `src/locator/*` |
| 4 | Provider / SSE / Reasoning | `src/ai/deepseek_chat_client.cpp`, `chat_provider.h` |
| 5 | AssistantPanel / Preset / Menu / 线程 | `src/ui/assistant_panel.{h,cpp}`, `src/ai/preset_*` |
| 6 | 持久化 / 生命周期 | `src/storage/*`, `src/plugin/plugin_callbacks.cpp` |
| 7 | Agent 驱动调试器的 SDK 扩展点 | `third_party/pluginsdk/*` |

证据均已落到上文相应章节的 `file:line`，可直接核对。

---

*Report generated: M4.7-review-2026-05-24. Pending user triage.*
