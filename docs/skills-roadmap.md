# 技能与工作流路线图

> 单一事实源（SSOT），跟踪 AI agent 的"技能"（工具）和"工作流"（预设）的实现状态。
> **每次完成一个 S 段后，更新本表的 ✅/❌ 列 + Done 行**。
> 横向对照：[features.md](features.md) 是最终能力快照；本文件是规划+进度。

最后更新：2026-05-25（**S9 完成** = G-10 工具与预设管理 UI 重构 + 分类系统 + 工具描述中文化）

---

## 1. 工具（"技能"）矩阵

### 1.1 已实现（63 个，✅ = 真机可用）

| # | 类别 | 工具 | 状态 | 能力一句话 | SDK API | 阶段 |
|---|---|---|---|---|---|---|
| 1 | 静态读 | `get_disasm` | ✅ | 反汇编 N 行 | `DbgDisasmAt` | M3 |
| 2 | 静态读 | `read_memory` | ✅ | 读 1B/2B/4B/8B/hex，单次 ≤64 KB | `DbgMemRead` | M3 |
| 3 | 静态读 | `read_string` | ✅ | 自动判 ASCII/UTF-16 抓字符串 | `DbgGetStringAt` | M3 |
| 4 | 静态读 | `get_registers` | ✅ | 通用寄存器快照 | `DbgGetRegDumpEx` | M3 |
| 5 | 静态读 | `list_modules` | ✅ | 已加载模块 base/size/path | `Script::Module::GetList` | M3 |
| 6 | 静态读 | `eval_expression` | ✅ | x64dbg 表达式求值 | `DbgEval` | S1 |
| 7 | 静态读 | `list_breakpoints` | ✅ | 当前所有断点 | `DbgGetBpList` | S1 |
| 8 | 静态分析 | `find_xrefs_to` | ✅ | 找谁引用某地址 | `DbgXrefGet` | M3 |
| 9 | 静态分析 | `get_function_range` | ✅ | 函数起止+大小 | `DbgFunctionGet` | M3 |
| 10 | 静态分析 | `search_pattern` | ✅ | hex/通配 pattern 搜内存 | `Script::Pattern::FindMem` | M3 |
| 11 | 动态/项目 | `get_callstack` | ✅ | 当前调用栈帧 | `DbgFunctions()->GetCallStack` | M3 |
| 12 | 动态/项目 | `trace_query` | ✅ | 跟踪记录查询 | 自研 SessionStore | M3 |
| 13 | 动态/项目 | `locate_api_callers` | ✅ | 找某 API 所有调用点 | 自研（IAT+xref） | M3 |
| 14 | 动态/项目 | `rag_search` | ✅ | 项目级语义检索 | sqlite-vec | M3 |
| 15 | 调试控制 | `wait_for_event` | ✅ | 阻塞等 Paused/BP/Running | `Script::Debug::Wait` | S2 |
| 16 | 调试写 | `set_breakpoint` | ✅ | 软件断点设置 | `Script::Debug::SetBreakpoint` | S3 |
| 17 | 调试写 | `remove_breakpoint` | ✅ | 软件断点删除 | `Script::Debug::DeleteBreakpoint` | S3 |
| 18 | 调试写 | `step_in` | ✅ | 单步进入 | `Script::Debug::StepIn` | S3 |
| 19 | 调试写 | `step_over` | ✅ | 单步跳过 | `Script::Debug::StepOver` | S3 |
| 20 | 调试写 | `run_until` | ✅ | 跑到目标 VA | `DbgCmdExec("run <va>")` | S3 |
| 21 | 调试写 | `run_dbg_command` | ✅ | 命令逃生口（白名单） | `DbgCmdExec` | S3 |
| 22 | 数据写 | `patch_memory` | ✅ | hex 字节流补丁 ≤4 KB | `DbgMemWrite` | S4 |
| 23 | 数据写 | `set_register` | ✅ | GPR/DR/EFLAGS/子寄存器 | `Script::Register::Set` | S4 |
| 24 | 数据写 | `write_string` | ✅ | utf8/utf16le/ascii 自动 \0 | `DbgMemWrite` | S4 |
| 25 | 脚本 | `list_scripts` | ✅ | 列 scripts/ 目录 | `std::filesystem` | S5 |
| 26 | 脚本 | `load_script` | ✅ | 加载到 Script tab 不执行 | `DbgScriptLoad` | S5 |
| 27 | 脚本 | `run_script_file` | ✅ | Load + Run（fire-and-forget） | `DbgScriptRun(0)` | S5 |
| 28 | 调试控制 | `run_continue` | ✅ | 继续运行（默认 fire-and-forget；wait_for_stop=true 阻塞） | `Script::Debug::Run` + `Wait` | S6 |
| 29 | 调试控制 | `pause_debug` | ✅ | 异步打断 + 等 Paused（5s 超时） | `Script::Debug::Pause` + `Wait` | S6 |
| 30 | 调试写 | `step_out` | ✅ | 跳出当前函数（同步等 30s） | `Script::Debug::StepOut` + `Wait` | S6 |
| 31 | 沉淀 | `set_label` | ✅ | 设置/删除（text=""）标签，写入 .dd64 | `Script::Label::Set/Delete` | S6 |
| 32 | 沉淀 | `get_label` | ✅ | 读单个 VA 的标签 | `Script::Label::Get` | S6 |
| 33 | 沉淀 | `list_labels` | ✅ | 全量标签列表 | `Script::Label::GetList` | S6 |
| 34 | 沉淀 | `set_comment` | ✅ | 设置/删除（text=""）注释，写入 .dd64 | `Script::Comment::Set/Delete` | S6 |
| 35 | 沉淀 | `get_comment` | ✅ | 读单个 VA 的注释 | `Script::Comment::Get` | S6 |
| 36 | 沉淀 | `list_comments` | ✅ | 全量注释列表 | `Script::Comment::GetList` | S6 |
| 37 | 内存映射 | `get_memory_map` | ✅ | 全量内存映射（base/size/state/RWX/info） | `DbgMemMap` | S6 |
| 38 | 内存映射 | `get_page_protect` | ✅ | 读单 VA 的页保护 | `Script::Memory::GetProtect` | S6 |
| 39 | 内存映射 | `set_page_protect` | ✅ | 改区段 RWX（Write+confirm） | `Script::Memory::SetProtect` | S6 |
| 40 | 程序地图 | `list_functions` | ✅ | 所有已分析函数（可按模块过滤） | `Script::Function::GetList` | S6 |
| 41 | 程序地图 | `get_module_imports` | ✅ | 指定模块 IAT 列表 | `Script::Module::GetImports` | S6 |
| 42 | 程序地图 | `get_module_exports` | ✅ | 指定模块 EAT 列表 | `Script::Module::GetExports` | S6 |

**小计**：15(原读) + 6(S6 新读：3 label + 3 comment) + 5(S6 新读：memory_map/page_protect/list_functions/imports/exports) + 1(原控制) + 2(S6 新控制：run_continue/pause_debug) + 11(原写) + 2(S6 新写：step_out/set_page_protect) + 1(S6 沉淀写 set_label) + 1(S6 沉淀写 set_comment) + 7(S7 写：set_hw_breakpoint/remove_hw_breakpoint/set_conditional_bp/assemble_at/pattern_replace/set_flag/restore_patch) + 1(S7 写 assemble_at 已计入 7 中) + 3(S7 读：get_cfg/list_patches/format_with_dbg) + 2(S7 控制：gui_focus_disasm/gui_focus_dump) = **49**
（更直白：29 读 + 5 控制 + 15 写 = 49）

### 1.2 计划中（按 ROI 排序）

#### P0（S6 已完成 ✅ — 见 1.1 表第 28–42 行）

#### P1（S7 已完成 ✅ — 12 个工具实装；2026-05-24，tag `s7-done`）

| # | 类别 | 工具 | 状态 | 能力一句话 | SDK API | 阶段 |
|---|---|---|---|---|---|---|
| 43 | 高级断点 | `set_hw_breakpoint` | ✅ | 硬件断点 execute/write/access；4 槽上限 | `Script::Debug::SetHardwareBreakpoint(addr,HardwareType)` | S7 |
| 44 | 高级断点 | `remove_hw_breakpoint` | ✅ | 删除指定 VA 的 HWBP | `Script::Debug::DeleteHardwareBreakpoint` | S7 |
| 45 | 高级断点 | `set_conditional_bp` | ✅ | 在既有软断点上配 break/log/command + 各自 condition + fastResume/silent；空串清字段 | `BpRefVa` + `BpSetFieldText/Number(bpf_*)` | S7 |
| 46 | 汇编 | `assemble_at` | ✅ | 在 VA 汇编一条指令；默认 fill_nop=true；返回汇编后字节数 | `Script::Assembler::AssembleMemEx` | S7 |
| 47 | 模式 | `pattern_replace` | ✅ | [start,start+size) 内一行批量替换；size≤16MB；`??` 通配 | `Script::Pattern::SearchAndReplaceMem` | S7 |
| 48 | CFG | `get_cfg` | ✅ | DbgAnalyzeFunction → BridgeCFGraph → Mermaid graph TD；256 节点截断 | `DbgAnalyzeFunction` + `bridgegraph.h` | S7 |
| 49 | 标志 | `set_flag` | ✅ | 设置 ZF/OF/CF/PF/SF/TF/AF/DF/IF | `Script::Flag::Set` | S7 |
| 50 | 补丁 | `list_patches` | ✅ | 列已打补丁（PatchEnum 两阶段），可按模块名子串过滤 | `DbgFunctions()->PatchEnum` | S7 |
| 51 | 补丁 | `restore_patch` | ✅ | 回滚指定 VA 的单字节补丁 | `DbgFunctions()->PatchRestore` | S7 |
| 52 | 格式化 | `format_with_dbg` | ✅ | x64dbg 原生模板 `{x:[rax+8]}`；4KB 输出 | `DbgFunctions()->StringFormatInline` | S7 |
| 53 | GUI | `gui_focus_disasm` | ✅ | 滚动反汇编视图到 VA（DbgControl，无副作用） | `GuiDisasmAt` | S7 |
| 54 | GUI | `gui_focus_dump` | ✅ | 滚动 dump 视图到 VA；可选 index∈[1,5] 选 Dump1..5 | `GuiDumpAt` / `GuiDumpAtN` | S7 |

#### P2 场景化（S8 已完成 ✅ — 13 个工具实装；2026-05-24，tag `s8-done`）

| # | 类别 | 工具 | 状态 | 能力一句话 | SDK API | 阶段 |
|---|---|---|---|---|---|---|
| 55 | 反调试洞察 | `list_threads` | ✅ | 枚举所有线程：TID/CIP/SuspendCount/Priority/WaitReason/UserTime/KernelTime | `DbgGetThreadList` + 手动 `BridgeFree(list.list)` | S8 |
| 56 | 反调试洞察 | `get_peb_address` | ✅ | 取主进程或指定线程的 PEB；可选 read_bytes 同时返回前 N 字节 | `DbgGetPebAddress` | S8 |
| 57 | 反调试洞察 | `get_anti_debug_flags` | ✅ | 一键诊断 PEB.BeingDebugged / NtGlobalFlag / ProcessHeap，按 ptr_size 切偏移（x86 +0x68/+0x18 vs x64 +0xBC/+0x30） | `DbgGetPebAddress` + `DbgMemRead` | S8 |
| 58 | 取证 | `enum_handles` | ✅ | 枚举句柄；两阶段 `EnumHandles` + `GetHandleName` 拿 type/name；支持 type_filter CI 子串 | `DbgFunctions()->EnumHandles/GetHandleName` | S8 |
| 59 | 取证 | `enum_windows` | ✅ | 枚举窗口；handle/title/class/threadId/style/styleEx/wndProc/位置 | `DbgFunctions()->EnumWindows` | S8 |
| 60 | 取证 | `enum_tcp_connections` | ✅ | C2 取证：本地/远端 IP:Port + 状态字串 | `DbgFunctions()->EnumTcpConnections` | S8 |
| 61 | 异常 | `get_seh_chain` | ✅ | x86 链表 SEH；x64 返空 + hint 引向 .pdata/RtlLookupFunctionEntry；用 `if constexpr` 规避 C4127 | `DbgFunctions()->GetSEHChain` + `BridgeFree(records)` | S8 |
| 62 | 注入 | `remote_alloc` | ✅ | 在被调试进程分配 VirtualAlloc 区；SDK 内部固定 PAGE_EXECUTE_READWRITE；64MB 上限 | `Script::Memory::RemoteAlloc` | S8 |
| 63 | 注入 | `remote_free` | ✅ | 释放 remote_alloc 返回的基址（不能传中间页） | `Script::Memory::RemoteFree` | S8 |
| 64 | 栈 | `stack_push` | ✅ | 压栈 duint；ESP/RSP -= ptr_size；返回压栈前的 top + 新 SP | `Script::Stack::Push` + `Script::Register::GetCSP` | S8 |
| 65 | 栈 | `stack_peek` | ✅ | 读 [SP+offset×ptr_size]；offset 单位是 SLOT 不是字节（坑） | `Script::Stack::Peek` + `GetCSP` | S8 |
| 66 | trace | `get_trace_record_info` | ✅ | 合并 hit_count + byte_type + page record_type（None/BitExec/.../WordWithExec）；hint 引导 enable | `GetTraceRecordHitCount/ByteType/Type` | S8 |
| 67 | 翻译 | `translate_error_code` | ✅ | 0xC0000005 → EXCEPTION_ACCESS_VIOLATION；lazy-init unordered_map 全表（EnumErrorCodes + EnumExceptions），后续 O(1) | `EnumErrorCodes` + `EnumExceptions` + `std::call_once` | S8 |
| 68 | 函数 | `add_function` | ✅ | 注册函数区间；end 是最后一条指令 VA inclusive（不是 end+1）；manual=true 默认 | `Script::Function::Add(start,end,manual)` | S8 |

#### P3 后续场景化（暂无）

### 1.3 不包装（设计取舍）

| API / 类别 | 不做的理由 |
|---|---|
| `Bridge*Init/Start` / `Dbg*Init/Exit` / `BridgeAlloc/Free` | 调试器自身生命周期 / 内部分配器 |
| `BridgeSetting*` | 改 x64dbg 全局配置，副作用大 |
| `GuiExecuteOnGuiThread` / `DbgWinEvent*` | Qt 消息循环底层，错用死锁 |
| `GuiAddQWidgetTab` / `GuiTypeAddNode` | 需要 Qt `QWidget*`，跨语言无法构造 |
| `GuiMenu*` / `GuiAddFavourite*` | 插件菜单系统，与 agent 无关 |
| `GuiCloseApplication` / `DbgSetJit` | 极端危险 |
| `DbgScriptStep/Abort/BpToggle/...` | 给 x64dbg 自家脚本编辑器用，agent 只需 Load+Run |
| `DbgGetEncodeType*` / `DbgTypeVisit` 等 | 字节显示编码 / 类型 widget，纯 GUI 渲染 |
| jansson / lz4 直接暴露 | 已有 nlohmann / 自研，无需透传 |
| `ValueFromString` / `GetPrivilegeList` / `VectoredHandler*` | SDK 中**不存在**（S8 探查确认；ValueFromString 应用 `ValFromString`；权限列表/VEH 链无公开 API） |
| `stack_pop` | 真弹出会破坏 ESP 一致性，反向工程几乎用不到；S8 决定不暴露，需要时让 LLM 用 stack_peek + set_register 显式做 |
| `enum_constants` 全量 | 只暴露 `translate_error_code`（按值查名）；全量枚举对 LLM 是噪音 |

---

## 2. 工作流预设（"预设"）矩阵

### 2.1 已实现（14 个）

| # | 预设 ID | 状态 | 中文名 | 启用工具 | 上下文菜单 | maxIter | 阶段 |
|---|---|---|---|---|---|---|---|
| 1 | `freeform` | ✅ | 自由 Agent | 全 62 | 否 | 20 | M4 |
| 2 | `analyze-function` | ✅ | 分析当前函数 | 全 62（含写+脚本+S6 沉淀+地图+S7 高级+S8 全部 13） | 是 | 20 | M4 + S3-S8 持续扩 |
| 3 | `who-calls-here` | ✅ | 谁调用了这里 | 8（纯读） | 是 | 20 | M4 |
| 4 | `string-api-context` | ✅ | 字符串与 API 关联 | 7（纯读） | 是 | 20 | M4 |
| 5 | `explain-here` | ✅ | 解释此处 | 5（纯读） | 是 | 6 | M4 |
| 6 | `annotate-function` | ✅ | 标注当前函数 | 13（读 11 + 写 set_label/set_comment） | 是 | 20 | S6 |
| 7 | `map-program` | ✅ | 程序地图 | 8（纯读：list_modules + memory_map + page_protect + list_functions + imports + exports + list_labels + rag_search） | 是 | 20 | S6 |
| 8 | `crack-license` | ✅ | 破解许可校验 | 21（定位+注释+控制+set_flag+patch+审计） | 是 | 20 | S7 |
| 9 | `anti-anti-debug` | ✅ | 反反调试 | 30+（S8 后扩了 get_anti_debug_flags / list_threads / enum_handles / enum_windows 共 5 个被动诊断工具，systemPrompt 增"先 passive 后 active"） | 是 | 20 | S7→S8 增强 |
| 10 | `cfg-explorer` | ✅ | 控制流图探索 | 12（CFG + 标注查询） | 是 | 12 | S7 |
| 11 | `patch-and-verify` | ✅ | 补丁与验证 | 20（list/restore + 备份-修改-验证流程） | 是 | 20 | S7 |
| 12 | `trace-input` | ✅ | 追踪输入数据 | 21（HW write BP + wait_for_event + dump 焦点） | 是 | 20 | S7 |
| 13 | `malware-triage` | ✅ | 恶意代码取证 | 28（纯只读 + 仅允许 label/comment 沉淀；S8-A/B/C 全部 + translate_error_code） | 是 | 25 | S8 |
| 14 | `unpack-helper` | ✅ | 脱壳辅助 | 24（HW BP + run_continue + get_trace_record_info + add_function + stack_peek） | 是 | 30 | S8 |

### 2.2 计划中

#### S6 目标（已完成 ✅ — 见 2.1 表第 6–7 行）

#### S7 目标（已完成 ✅ — 见 2.1 表第 8–12 行；tag `s7-done`）

#### S8 目标（已完成 ✅ — 见 2.1 表第 13–14 行；anti-anti-debug 同步扩 5 工具；tag `s8-done`）

| # | 预设 ID | 状态 | 备注 |
|---|---|---|---|
| 13 | `malware-triage` | ✅ | 纯只读 + 仅 label/comment 沉淀；S8-A/B/C + translate_error_code |
| 14 | `unpack-helper` | ✅ | HW BP + trace_record + add_function + stack_peek |
| —  | `anti-debug-bypass`（原计划） | 合并 | 与 `anti-anti-debug` 语义重叠，决策为扩 anti-anti-debug 加 PEB 被动诊断 |
| —  | `decrypt-loop-runner` | 不做 | 用户可用 freeform + load_script 自由组合 |

---

## 3. 路线图

| 阶段 | 目标 | 新工具数 | 新预设数 | preset schema | 状态 |
|---|---|---|---|---|---|
| S0 | 基线 | 0 | 0 | — | ✅ `s0-baseline` / `s0-done` |
| S1 | 表达式 + 断点列举 | 2 | 0 | v5 | ✅ `s1-done` |
| S2 | 事件总线 + wait_for_event | 1 | 0 | v6 | ✅ `s2-done` |
| S3 | ToolPolicy + 5s confirm + audit + 6 个写工具 | 6 | 0 | v7 | ✅ `s3-done` |
| S4 | 数据写三件套 | 3 | 0 | v8 | ✅ `s4-done` |
| S5 | 脚本三件套 | 3 | 0 | v9 | ✅ `s5-done` |
| **S6** | **基础控制 + 沉淀（label/comment） + 程序地图** | **10** | **2** | **v10** | ✅ `s6-done` |
| **S7** | **高级断点 + 汇编 + CFG + 补丁管理 + GUI 焦点** | **12** | **5** | **v11** | ✅ `s7-done` |
| **S8** | **场景化（反调试洞察 / 取证 / SEH / 注入+栈 / trace+错误码+函数）** | **13** | **2**(+扩 anti-anti-debug) | **v12** | ✅ `s8-done` |

---

## 4. 维护规则

1. **每完成一个 S 段**：把对应行 ❌ 改 ✅，更新 `阶段` 列；表 3 路线图末行勾上 tag。
2. **新增工具/预设**：在对应 1.2 / 2.2 表里插入新行，状态 ❌，标好预计阶段。
3. **取消工具/预设**：从表中删除并在 1.3 / 设计取舍区记原因。
4. **schema 升级**：表 3 必记，并同步 `kPresetSchemaVersion` 与 features.md。
5. 本文件是规划+进度，**详细技术细节**仍写在 features.md / known-issues.md / development-log.md。

---

## 5. 工具治理（待办，独立小段，不阻塞 S6/S7/S8）

> 背景：工具数从 27 涨到 57 后，每次 chat completion 的 input tokens 会从 ~4k 涨到 ~8k，
> 影响成本（DeepSeek 单次会话 $0.02→$0.04）和首 token 延迟（多 1-2 秒）。
> 已有防御：**预设白名单**（`enabledTools` 只透传勾选的工具）+ `maxIter=20`。
> 用户决定：**先推进 S6/S7/S8 加技能，治理后续做**。

### 5.1 治理任务清单

| # | 任务 | 优先级 | 状态 | 触发时机 |
|---|---|---|---|---|
| G-1 | 复查现有 27 工具 description 长度，全部砍到 ≤80 字 + 关键限制 | P1 | ❌ | S7 完成后 |
| G-2 | 验证 DeepSeek / Copilot prompt caching 是否启用（system prompt + tools 缓存命中后 input 价 ÷10） | P0 | ❌ | S6 完成后立即查 |
| G-3 | `AgentWorker` 加 `maxToolCalls=30` 配置（与 maxIter 区分） | P1 | ❌ | S7 |
| G-4 | system prompt 加"同义工具决策树"（step_in/over/out/run_until/run_continue 何时用谁） | P1 | ❌ | S6 完成后 |
| G-5 | PresetEditor UI 加"代价提示"：勾工具时显示"预计 +X tokens/轮"（**并入 G-10**） | P2 | ✅ | S9 |
| G-6 | PresetEditor UI 加"工具分组开关"：按 category 整组勾选（**并入 G-10**） | P2 | ✅ | S9 |
| G-7 | write_audit.log 旁加 metrics 计数（read 工具不写大日志，只记总次数） | P3 | ❌ | 视需要 |
| G-8 | （研究类）动态工具子集：第一轮 `request_tools(["category"])` 按需解锁 | P3 | ❌ | 实现复杂，暂不做 |
| G-9 | 工具/预设描述与 userTemplate 中文化（详情见 5.3） | P2 | ✅（A 档 · UI only） | S9 |
| **G-10** | **工具与预设管理界面重构 + 分类系统**（详情见 5.4） | **P1** | ✅ | **S9 完成 2026-05-25** |

### 5.2 治理原则（S6/S7/S8 实施时附带遵守）

1. **新工具 description ≤80 字**（已有 27 个不动，留给 G-1 统一过）
2. **新预设严格白名单**：`map-program` 只开程序地图相关，不开写工具；不复制 `analyze-function` 的全集
3. **同义/互斥工具在 description 里互相提示**（如新加的 `run_continue` 在 description 里写"step 类工具优先；只需要继续运行才用本工具"）
4. **写工具继续依赖 5s confirm**（已有 ToolPolicy 兜底，治理层不重复造防御）

### 5.3 G-9 工具/预设中文化（决策记录）

S6 期间用户提出"全量中文化"诉求。结论 **暂不做，先收尾 S6**；列入 G-9 待 S7 之后单独评估。
评估时按下面三档逐项过：

| 字段 | 当前 | 中文化建议 | 理由 |
|---|---|---|---|
| 工具 `name()` | 英文 ASCII | **保持英文** | OpenAI/DeepSeek function-call schema 硬约束 `^[a-zA-Z0-9_-]{1,64}$`，中文 name 会被 400 拒绝 |
| 工具 `description()` | 英文 | 可中文化（A 档） | LLM 能理解，但中文 token 占用 ~+30%，且会重置 prompt cache（先做 G-2 验证） |
| 参数 schema `description` | 英文 | 可中文化（A 档） | 同上 |
| 预设 `id` | 英文 kebab-case | **保持英文** | 持久化 key，配置文件/审计日志引用 |
| 预设 `name` / `description` | 中文 | 已是中文 | — |
| 预设 `systemPrompt` | 英文 + 末尾 zh-CN 输出强制 | **保持英文**（C 档不建议） | 实测 LLM 对英文指令服从度更稳；改中文需充分回归 |
| 预设 `userTemplate` | 英文模板 | 可中文化（B 档） | 用户感知，影响较小 |

实施前置条件：**G-2 验证 prompt cache 启用情况后再决定 A/B 档**。

### 5.4 G-10 工具与预设管理界面重构 + 分类系统（新需求 2026-05-24）

**背景**：S7 后工具数 49、预设数 12，PresetEditor 现状是一个长 QListWidget 平铺所有工具勾选框 + 一个平铺预设列表；继续增长（S8 预计再加 8–12 个工具 + 3–5 预设）会让用户在勾选 / 浏览 / 找预设时严重失焦。

**目标**：
1. 工具分类（categorization）— 在数据层给每个工具加 `group` 字段（**与现有 `ToolCategory`=Read/DbgControl/Write 正交，是"功能域"分组**），UI 用分组渲染。
2. 预设分类 — 给每个预设加 `tags`（多标签）+ `group`（单分组），UI 用左侧分组导航 + 右侧详情。
3. 工具勾选 UI 重做 — 分组折叠面板 + 组级"全选/反选"+ 顶部搜索框 + 顶部多 chip 过滤（按 Read/Write/分组）。
4. 预设列表 UI 重做 — 左侧分组树（场景：破解/反反调试/初探/全能/自定义）+ 右侧卡片视图（含标签、工具数、provider、只读锁）。
5. 兼容性 — `group` / `tags` 都是新增可选字段，老配置启动时**按工具 name 前缀+关键字自动推断**初值，不要求用户手动迁移。

**工具分组方案**（10 组，覆盖现 49 个）：

| group | 工具示例 | 数量 |
|---|---|---|
| `static-info` | get_module_info / list_modules / get_module_imports / get_module_exports / get_section_info | ~6 |
| `disasm-cfg` | get_disasm / get_function_range / list_functions / get_cfg | ~4 |
| `memory-search` | read_memory / search_memory / pattern_search / get_strings / get_memory_map / get_page_protect | ~6 |
| `register-stack` | get_registers / get_stack / get_thread_list | ~3 |
| `breakpoint` | set_breakpoint / remove_breakpoint / list_breakpoints / set_hw_breakpoint / remove_hw_breakpoint / set_conditional_bp | ~6 |
| `execution-control` | run_continue / pause_debug / step_into / step_over / step_out / run_until / wait_for_event | ~7 |
| `annotation` | set_label / get_label / list_labels / set_comment / get_comment / list_comments | ~6 |
| `write-patch` | write_memory / assemble_at / pattern_replace / set_flag / set_page_protect / list_patches / restore_patch | ~7 |
| `gui-misc` | gui_focus_disasm / gui_focus_dump / format_with_dbg / run_dbg_command | ~4 |
| `agent-meta` | rag_search / list_scripts / load_script / run_script_file / locate_api_callers | ~5 |

> 实施时按当前真实工具列表回拢，每组期望 3–8 个，过大就拆。

**预设分组方案**（4 组）：

| group | 预设 | 说明 |
|---|---|---|
| `general` | freeform / analyze-function / explain-here | 通用与全能 |
| `exploration` | map-program / cfg-explorer / who-calls-here / string-api-context | 只读探索 |
| `cracking` | crack-license / patch-and-verify / anti-anti-debug | 写 / 补丁类 |
| `tracing` | trace-input / annotate-function | 动态追踪与沉淀 |

**预设标签**（与 group 正交，多选）：`read-only` / `write` / `hw-bp` / `cfg` / `patch` / `annotation` / `dataflow` / `anti-debug`。UI 可按 tag 二级筛。

**Schema 影响**：
- `Tool` 基类加 `virtual std::string group() const { return "agent-meta"; }`（默认值兜底，子类按需 override）。
- `AgentPreset` 加 `std::string group;` + `std::vector<std::string> tags;`；`kPresetSchemaVersion` 11 → 12；老盘文件无字段时启动期按 id 推断填充并回写。

**UI 改造点**（PresetEditor.cpp / PresetListPanel）：
- 工具勾选：`QListWidget` → `QTreeWidget`（组节点 + 子工具节点 + 三态勾选）；顶部 `QLineEdit` 名称搜索 + 一排 `QToolButton` chip 过滤（Read/DbgControl/Write/各 group）。
- 预设列表：左侧 `QTreeView`（group → 预设）；右侧 `QStackedWidget` 卡片视图。
- 工具数/估算 token（G-5）合并进卡片右下角 badge。
- 组级"全选/反选/反选 Read-only"按钮（G-6）。

**验收**：
1. 49 工具按 group 渲染成 10 个折叠组；搜索 "hw" 能立刻定位到 set_hw_breakpoint。
2. 12 预设左侧分 4 组，"crack-license" 在 `cracking` 组下；右侧卡片显示 "5 tools · cracking · write,patch · deepseek · 🔒"。
3. 老 `agent_presets.json`（schema 11）启动自动迁移到 12，无需用户介入。
4. 新建预设默认 group=`general`、tags=[]，可在 UI 编辑。

**拆分到 S9（独立阶段）执行**：S8 先把 P2 场景化工具实现完，G-10 紧接 S8 后作为 **S9 = "UI + 分类治理"** 单独立项，避免和工具堆积纠缠。

### 5.5 S9 落地实况（2026-05-25 完成）

**实际工具规模**：63 工具 / 注册条目 68 / 预设 14 / schema 13。

**完成项**：
- **数据层**：`ToolRegistry` 增 `groupOf / listGroups / listToolsByGroup / categoryOf` 4 个 API；`AgentPreset` schema 12 → 13，加 `group` + `tags` + `readonly`（出厂 14 预设全 `readonly=true`）。
- **PresetEditor 重做**（`preset_editor_dialog.cpp`）：
  - 左侧 `QTreeView`（group → 预设），右侧详情卡
  - 工具勾选改 `QTreeWidget` 分组折叠 + 三态勾选 + 顶部搜索 + Read/Ctrl/Write chip 过滤
  - 卡片显示 tools 数、provider、tags、🔒 锁标
  - 出厂预设可"解锁副本"派生为可编辑用户预设
- **新增「工具列表」对话框**（`tools_browser_dialog.h/.cpp`）：从 popup menu 改造为只读 4 列树（工具名 / 类别徽标 / 描述 / 当前预设启用 ✓✗），顶部 badge + 搜索 + chip + 「只显示当前预设启用」复选框；双击叶子展示自带 schema 详情。
- **G-9 工具描述中文化（A 档 · UI only）**：`ITool` 加 `virtual std::string descriptionZh()`（默认 fallback 英文）；`ChatTool` 加 `descriptionZh` 字段。所有 63 工具补齐中文 override。**LLM 仍读英文 description**，UI 显示优先中文，避免破坏 prompt cache 与 system prompt 英文一致性。
- **dark 主题修复**：`theme_dark.qss` 追加 `QDialog` 子树控件段（QLabel/QLineEdit/QSpinBox/QCheckBox/QGroupBox/QTreeWidget/QHeaderView::section/QDialogButtonBox 等）；两个对话框打开前 `setStyleSheet(":/x64dbg-ai/styles/theme_dark.qss")` 注入。
- **UI 文案统一**：主界面 `Agent` 按钮 → 「工作流」；菜单「管理预设…」→「管理工作流…」；工具列表底部「打开预设编辑器…」→「打开工作流编辑器…」（内部数据结构 `AgentPreset` 名称未改，仅 UI 字面）。

**未做 / 推迟**：
- G-5 token 估算 badge：UI 卡片暂只显示 "N tools"，未做 token 估算（依赖 G-2 prompt-cache 验证后才有意义）
- G-7 metrics 计数：低优先级延后
- G-8 动态工具子集：复杂，不做
- 预设 `userTemplate` 中文化（G-9 B 档）：本阶段未涉及，待回归后单独评估
