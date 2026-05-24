# 技能与工作流路线图

> 单一事实源（SSOT），跟踪 AI agent 的"技能"（工具）和"工作流"（预设）的实现状态。
> **每次完成一个 S 段后，更新本表的 ✅/❌ 列 + Done 行**。
> 横向对照：[features.md](features.md) 是最终能力快照；本文件是规划+进度。

最后更新：2026-05-24（S7 完成；S8 未启动）

---

## 1. 工具（"技能"）矩阵

### 1.1 已实现（49 个，✅ = 真机可用）

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

#### P2 场景化（S8+ 按需）

| # | 类别 | 工具 | 状态 | 场景 | SDK API | 阶段 |
|---|---|---|---|---|---|---|
| 53 | 反调试 | `get_peb_address` / `get_thread_list` | ❌ | 读 PEB.BeingDebugged | `DbgGetPebAddress` / `DbgGetThreadList` | S8 |
| 54 | 恶意软件 | `enum_handles` | ❌ | 列打开的文件/互斥体 | `DbgFunctions()->EnumHandles` | S8 |
| 55 | 恶意软件 | `enum_tcp_connections` | ❌ | C2 连接 | `DbgFunctions()->EnumTcpConnections` | S8 |
| 56 | 异常 | `get_seh_chain` | ❌ | SEH 链分析 | `DbgFunctions()->GetSEHChain` | S8 |
| 57 | 注入 | `remote_alloc` / `remote_free` | ❌ | 注入 shellcode 区 | `Script::Memory::RemoteAlloc/Free` | S8 |
| 58 | 栈 | `stack_push` / `stack_pop` | ❌ | 栈修复 / 伪造返回地址 | `Script::Stack::Push/Pop` | S8 |
| 59 | trace | `get_trace_record_hits` | ❌ | 找代码热点 / 未执行路径 | `DbgFunctions()->GetTraceRecord*` | S8 |
| 60 | 翻译 | `enum_constants` / `error_code_to_name` | ❌ | `0xC0000005` → `ACCESS_VIOLATION` | `DbgFunctions()->EnumConstants` | S8 |
| 61 | 函数 | `set_function_range` | ❌ | 修正分析器漏识别的函数 | `Script::Function::Add` | S8 |
| 62 | 脚本 | `save_script` | ❌ | agent 写脚本到 scripts/（需 sandbox） | `std::filesystem` | S8 |

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

---

## 2. 工作流预设（"预设"）矩阵

### 2.1 已实现（7 个）

| # | 预设 ID | 状态 | 中文名 | 启用工具 | 上下文菜单 | maxIter | 阶段 |
|---|---|---|---|---|---|---|---|
| 1 | `freeform` | ✅ | 自由 Agent | 全 37 | 否 | 20 | M4 |
| 2 | `analyze-function` | ✅ | 分析当前函数 | 全 37（含写+脚本+S6 沉淀+地图） | 是 | 20 | M4 + S3/4/5/6 扩 |
| 3 | `who-calls-here` | ✅ | 谁调用了这里 | 8（纯读） | 是 | 20 | M4 |
| 4 | `string-api-context` | ✅ | 字符串与 API 关联 | 7（纯读） | 是 | 20 | M4 |
| 5 | `explain-here` | ✅ | 解释此处 | 5（纯读） | 是 | 6 | M4 |
| 6 | `annotate-function` | ✅ | 标注当前函数 | 13（读 11 + 写 set_label/set_comment） | 是 | 20 | S6 |
| 7 | `map-program` | ✅ | 程序地图 | 8（纯读：list_modules + memory_map + page_protect + list_functions + imports + exports + list_labels + rag_search） | 是 | 20 | S6 |

### 2.2 计划中

#### S6 目标（已完成 ✅ — 见 2.1 表第 6–7 行）

#### S7 目标（基于高级断点+汇编）

| # | 预设 ID | 状态 | 中文名 | 依赖新工具 | 用途 |
|---|---|---|---|---|---|
| 8 | `crack-license` | ❌ | License 破解 | `assemble_at` / 现有断点+patch | 找校验点 → patch jne→jmp |
| 9 | `anti-anti-debug` | ❌ | 反反调试 | `set_conditional_bp` / `pattern_replace` | 扫常见反调试 pattern + 自动 patch |
| 10 | `cfg-explorer` | ❌ | 控制流浏览 | `get_cfg` | CFG → mermaid，标识未执行块 |
| 11 | `patch-and-verify` | ❌ | 补丁验证 | `assemble_at` / `list_patches` / `restore_patch` | 写补丁 → 测试 → 失败自动回滚 |
| 12 | `trace-input` | ❌ | 数据追踪 | `set_hw_breakpoint` | 跟踪某缓冲区被谁读/写 |

#### S8 目标（场景化）

| # | 预设 ID | 状态 | 中文名 | 依赖新工具 | 用途 |
|---|---|---|---|---|---|
| 13 | `unpack-helper` | ❌ | 脱壳辅助 | `get_memory_map` / `set_page_protect` | 监控新建 RWX + EIP 进入 RWX → 自动 dump |
| 14 | `malware-triage` | ❌ | 恶意软件分流 | `enum_tcp_connections` / `enum_handles` / `get_seh_chain` | 列 C2 + 文件句柄 + 推测家族 |
| 15 | `decrypt-loop-runner` | ✅(可选) | 批量解密 | 已有 `load_script` / `run_script_file` | 用户预写 .script 跑 N 次解密，agent 收结果（**已可手工组合，无需新预设也行**） |

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
| **S7** | **高级断点 + 汇编 + CFG + 补丁管理** | **10** | **5** | **v11** | ❌ 未启动 |
| **S8** | **场景化（反调试 / 恶意软件 / 注入 / 脱壳）** | **10** | **2** | **v12** | ❌ 未启动 |

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
| G-5 | PresetEditor UI 加"代价提示"：勾工具时显示"预计 +X tokens/轮" | P2 | ❌ | S8 |
| G-6 | PresetEditor UI 加"工具分组开关"：按 category 整组勾选 | P2 | ❌ | S8 |
| G-7 | write_audit.log 旁加 metrics 计数（read 工具不写大日志，只记总次数） | P3 | ❌ | 视需要 |
| G-8 | （研究类）动态工具子集：第一轮 `request_tools(["category"])` 按需解锁 | P3 | ❌ | 实现复杂，暂不做 |
| G-9 | 工具/预设描述与 userTemplate 中文化（详情见 5.3） | P2 | ❌ | S7 之后单独评估 |

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
