# 技能与工作流路线图

> 单一事实源（SSOT），跟踪 AI agent 的"技能"（工具）和"工作流"（预设）的实现状态。
> **每次完成一个 S 段后，更新本表的 ✅/❌ 列 + Done 行**。
> 横向对照：[features.md](features.md) 是最终能力快照；本文件是规划+进度。

最后更新：2026-05-24（S5 完成；S6/S7/S8 未启动）

---

## 1. 工具（"技能"）矩阵

### 1.1 已实现（27 个，✅ = 真机可用）

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

**小计**：15 读 + 1 控制 + 11 写 = **27**

### 1.2 计划中（按 ROI 排序）

#### P0 基础缺口（S6 目标，10 个）

| # | 类别 | 工具 | 状态 | 能力一句话 | SDK API | 阶段 |
|---|---|---|---|---|---|---|
| 28 | 调试控制 | `run_continue` | ❌ | **继续运行**（目前只能走 run_dbg_command） | `Script::Debug::Run` | S6 |
| 29 | 调试控制 | `pause_debug` | ❌ | 异步打断长循环 | `Script::Debug::Pause` | S6 |
| 30 | 调试控制 | `step_out` | ❌ | 跳出当前函数 | `Script::Debug::StepOut` | S6 |
| 31 | 沉淀 | `set_label` / `get_label` / `list_labels` | ❌ | AI 命名子程序，跨会话持久化 | `Script::Label::*` | S6 |
| 32 | 沉淀 | `set_comment` / `get_comment` / `list_comments` | ❌ | AI 把语义注释写回反汇编 | `Script::Comment::*` | S6 |
| 33 | 内存映射 | `get_memory_map` | ❌ | 全量内存映射（可执行段/堆/栈） | `DbgMemMap` | S6 |
| 34 | 内存映射 | `get_page_protect` / `set_page_protect` | ❌ | 改 RWX 才能写 shellcode 区 | `Script::Memory::Get/SetProtect` | S6 |
| 35 | 程序地图 | `list_functions` | ❌ | LLM 一张程序地图 | `Script::Function::GetList` | S6 |
| 36 | 程序地图 | `get_module_imports` | ❌ | IAT 列表 / 识别 hook | `Script::Module::GetImports` | S6 |
| 37 | 程序地图 | `get_module_exports` | ❌ | 导出表 | `Script::Module::GetExports` | S6 |

#### P1 高 ROI（S7 目标，10 个）

| # | 类别 | 工具 | 状态 | 能力一句话 | SDK API | 阶段 |
|---|---|---|---|---|---|---|
| 38 | 高级断点 | `set_hw_breakpoint` / `remove_hw_breakpoint` | ❌ | 硬件断点 R/W/X 监控变量 | `Script::Debug::SetHardwareBreakpoint` | S7 |
| 39 | 高级断点 | `set_conditional_bp` | ❌ | 条件断点 + log 断点 + 命令断点三合一 | `BpSetFieldText(bpf_*)` | S7 |
| 40 | 汇编 | `assemble_at` | ❌ | 写汇编（"jne→jmp"），比手算字节准 | `Script::Assembler::AssembleMem` | S7 |
| 41 | 模式 | `pattern_replace` | ❌ | 一行批量替换 pattern | `Script::Pattern::SearchAndReplaceMem` | S7 |
| 42 | CFG | `get_cfg` | ❌ | 函数基本块+边，可序列化 mermaid | `DbgAnalyzeFunction` → `BridgeCFGraph` | S7 |
| 43 | 标志 | `set_flag` | ❌ | 强制改 ZF 让 je 跳走 | `Script::Flag::Set` | S7 |
| 44 | 补丁 | `list_patches` | ❌ | 列已打补丁 | `DbgFunctions()->PatchEnum` | S7 |
| 45 | 补丁 | `restore_patch` | ❌ | 回滚单个补丁 | `DbgFunctions()->PatchRestore` | S7 |
| 46 | 格式化 | `format_with_dbg` | ❌ | x64dbg 原生模板 `{x:[rax+8]}` | `DbgFunctions()->StringFormatInline` | S7 |
| 47 | GUI | `gui_focus_disasm` / `gui_focus_dump` | ❌ | 引导用户视线"看这里" | `GuiDisasmAt` / `GuiDumpAt` | S7 |

#### P2 场景化（S8+ 按需）

| # | 类别 | 工具 | 状态 | 场景 | SDK API | 阶段 |
|---|---|---|---|---|---|---|
| 48 | 反调试 | `get_peb_address` / `get_thread_list` | ❌ | 读 PEB.BeingDebugged | `DbgGetPebAddress` / `DbgGetThreadList` | S8 |
| 49 | 恶意软件 | `enum_handles` | ❌ | 列打开的文件/互斥体 | `DbgFunctions()->EnumHandles` | S8 |
| 50 | 恶意软件 | `enum_tcp_connections` | ❌ | C2 连接 | `DbgFunctions()->EnumTcpConnections` | S8 |
| 51 | 异常 | `get_seh_chain` | ❌ | SEH 链分析 | `DbgFunctions()->GetSEHChain` | S8 |
| 52 | 注入 | `remote_alloc` / `remote_free` | ❌ | 注入 shellcode 区 | `Script::Memory::RemoteAlloc/Free` | S8 |
| 53 | 栈 | `stack_push` / `stack_pop` | ❌ | 栈修复 / 伪造返回地址 | `Script::Stack::Push/Pop` | S8 |
| 54 | trace | `get_trace_record_hits` | ❌ | 找代码热点 / 未执行路径 | `DbgFunctions()->GetTraceRecord*` | S8 |
| 55 | 翻译 | `enum_constants` / `error_code_to_name` | ❌ | `0xC0000005` → `ACCESS_VIOLATION` | `DbgFunctions()->EnumConstants` | S8 |
| 56 | 函数 | `set_function_range` | ❌ | 修正分析器漏识别的函数 | `Script::Function::Add` | S8 |
| 57 | 脚本 | `save_script` | ❌ | agent 写脚本到 scripts/（需 sandbox） | `std::filesystem` | S8 |

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

### 2.1 已实现（5 个）

| # | 预设 ID | 状态 | 中文名 | 启用工具 | 上下文菜单 | maxIter | 阶段 |
|---|---|---|---|---|---|---|---|
| 1 | `freeform` | ✅ | 自由 Agent | 全 27 | 否 | 20 | M4 |
| 2 | `analyze-function` | ✅ | 分析当前函数 | 全 27（含写+脚本） | 是 | 20 | M4 + S3/4/5 扩 |
| 3 | `who-calls-here` | ✅ | 谁调用了这里 | 8（纯读） | 是 | 20 | M4 |
| 4 | `string-api-context` | ✅ | 字符串与 API 关联 | 7（纯读） | 是 | 20 | M4 |
| 5 | `explain-here` | ✅ | 解释此处 | 5（纯读） | 是 | 6 | M4 |

### 2.2 计划中

#### S6 目标（基于沉淀类工具）

| # | 预设 ID | 状态 | 中文名 | 依赖新工具 | 用途 |
|---|---|---|---|---|---|
| 6 | `annotate-function` | ❌ | 注释沉淀 | `set_label` / `set_comment` | 分析后把注释/标签写回 x64dbg，跨会话累积 |
| 7 | `map-program` | ❌ | 程序总览 | `list_functions` / `get_memory_map` / `get_module_imports` | 新样本第一步：函数表+IAT+段表 |

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
| **S6** | **基础控制 + 沉淀（label/comment） + 程序地图** | **10** | **2** | **v10** | ❌ 未启动 |
| **S7** | **高级断点 + 汇编 + CFG + 补丁管理** | **10** | **5** | **v11** | ❌ 未启动 |
| **S8** | **场景化（反调试 / 恶意软件 / 注入 / 脱壳）** | **10** | **2** | **v12** | ❌ 未启动 |

---

## 4. 维护规则

1. **每完成一个 S 段**：把对应行 ❌ 改 ✅，更新 `阶段` 列；表 3 路线图末行勾上 tag。
2. **新增工具/预设**：在对应 1.2 / 2.2 表里插入新行，状态 ❌，标好预计阶段。
3. **取消工具/预设**：从表中删除并在 1.3 / 设计取舍区记原因。
4. **schema 升级**：表 3 必记，并同步 `kPresetSchemaVersion` 与 features.md。
5. 本文件是规划+进度，**详细技术细节**仍写在 features.md / known-issues.md / development-log.md。
