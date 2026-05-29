# Agent 能力评估报告

> 日期：2026-05-28
> 评估对象：x64dbg-ai-plugin 内置 agent（`src/ai/agent_loop.cpp` + `src/ai/agent_worker.cpp` + `src/ai/tools/*`）
> 评估范围：工具调用编排、推理控制、上下文管理、错误恢复、知识检索集成
> 用途：为后续 backlog 排序提供事实依据；非营销材料

---

## 1. TL;DR

当前 agent 处于 **L2 末 / L3 初** 之间（按下方分级标准）：
- ✅ 工具调用循环、推理流式渲染、写工具 confirm/audit、cancel 都完整
- ✅ Tool surface 丰富（74 个工具 + 15 个出厂工作流预设）
- ✅ 单工具粒度的可观测性（write_audit.log + plugin.log + UI ToolCallCard 三层）
- ⚠️ 单一 ReAct 循环；tool calls 严格**串行执行**；没有上下文压缩、retry、并行 read、plan/critic 二级 agent
- ⚠️ RAG（case memory）在 agent 模式下**不自动注入**——LLM 必须显式调 `rag_search` 才能用到历史经验
- 与外部参照（Claude Code / Cursor agent / Cline）相比：tool surface 不输，**编排能力**差一个层级

按 ROI 排序，下批最值得做的 5 项改进（详见 §6）：
1. 失败工具自动 retry（最低成本，立竿见影）
2. RAG 在 agent 路径下基于 user task 自动检索注入（中等成本，质量提升大）
3. 上下文压缩（中等成本，长会话必需）
4. 只读工具并行执行（中等成本，速度 2-5×）
5. Plan-Execute 两阶段（高成本，复杂 task 必需）

> **落地进度（2026-05-29 更新）**：本报告 §7 原把 K-33~K-37 当占位编号，但实际开发中 K-33/K-34 已被其它工作占用（K-33=confirm 豁免、K-34=malware-triage 升级）。编排改进实际落在 **K-35**，且首批合并实现了**第 1 项（tool retry）+ 第 2 项（auto-RAG 注入）**两项：
> - retry 放 `AgentLoop` 层（非 dispatch decorator）：用文案白名单 `isTransientToolError` 判瞬时错误，仅非 Write 工具退避重试，默认 1 次
> - auto-RAG 在 run 入口注入一次：首条 user 问句 `embed` + `searchSimilar(top_k)` 拼成 system 消息
> - 两开关默认开（`tool_retry_enabled` / `auto_rag_inject_enabled`）
> - 详见 `known-issues.md::K-35` / `decisions.md 2026-05-29`
> 仍待做：第 3 项上下文压缩、第 4 项并行 read、第 5 项 Plan-Execute。

---

## 2. 分级标准（自定）

| 等级 | 名称 | 标志特征 |
|---|---|---|
| L1 | **单步问答** | LLM 输出文本回答，不调工具 |
| L2 | **ReAct 单循环** | LLM ↔ 工具串行多轮直到给最终答；无上下文管理、无重试、无并行 |
| L3 | **可扩展 ReAct** | L2 + ≥3 项：失败 retry / 并行 read / 上下文压缩 / 自动检索注入 / 长任务 checkpoint |
| L4 | **Plan-Execute / Multi-agent** | 二级 agent：planner 拆解 → executor 串行/并行执行 → critic/replan 形成闭环 |
| L5 | **自我演化** | 多 task 间共享 case memory + agent 能从历史成败学到 prompt/policy 改进，**无人工干预** |

参考点：Claude Code ≈ L3 末/L4 初；Cursor agent ≈ L3；Cline ≈ L3；早期 AutoGPT ≈ L4 雏形（但执行质量差）。

---

## 3. 现状：6 个维度逐项盘点

### 3.1 工具调用循环（ReAct loop）

**实现位置**：`src/ai/agent_loop.cpp::AgentLoop::run`

**机制**：
```
loop:
  provider.streamChat(messages, tools, this)   // 流式收 text/reasoning/tool_calls
  if (tool_calls.empty()) → DONE
  for each tool_call (序号顺序):
    dispatch(tool_call.name, tool_call.args)   // 阻塞
    messages.push({role: tool, tool_call_id: ..., content: result_json})
  if (++iter > max_iter) → STOP
```

**结论**：✅ 标准 ReAct，stream 体验良好；❌ tool_calls 严格按 provider 返回顺序**串行**执行，即使 LLM 同一 turn 返回 `[read_memory(A), read_memory(B), read_memory(C)]` 三个独立 read 也会一个接一个跑。

### 3.2 推理控制（max_iter / cancel / timeout）

| 项 | 现状 |
|---|---|
| `max_iter` | 默认 20，预设可配 1-50；超限直接 STOP 不再追问 |
| Cancel | UI「停止」按钮 → `AgentWorker.requestCancel()` → `ToolContext.cancelFlag` 工具级响应（wait_for_event/step_in/step_over/run_until 都已对接 50ms 切片轮询） |
| Tool 超时 | 仅调试控制类工具自带超时（默认 30s）；read/static 类无超时（除 SDK 自身阻塞外，没人会 hang 太久） |
| Provider 超时 | DeepSeek HTTP 客户端层有 connect/read 超时；reasoner 思考过长不会被打断（用户需手动 cancel） |

**结论**：✅ cancel 链路完整；⚠️ 缺「单 turn 软超时」——如果 LLM 在某 turn 推理 5 分钟，UI 卡 5 分钟，用户只能 cancel 不能"自动放弃当前 turn 继续"

### 3.3 上下文管理

**现状**：**零压缩**。`messages` 数组只增不减；每轮把完整历史塞回 provider。

**触发量**：
- 出厂预设的 system prompt ≈ 3-5K tokens（含 74 工具 schema）
- 单 read_memory 4KB 字节流 ≈ 1.5K tokens（hex 编码）
- 一次中等深度 task（30 轮工具调用，每轮平均 1K tokens 结果）≈ 30K tokens 工具历史 + system + user → 接近 DeepSeek 64K 上下文上限

**症状**：
- 长会话最后几轮会出现 LLM「忘了用户最初问什么」/「重复调用已经调过的工具」
- prompt cache hit ratio 高（G-2 已经能看），但**对幻觉无帮助**——cache 只省钱不省脑

**对比**：Claude Code 自动 summarize 旧 tool result；Cline 有 "context window full" UI 提示并触发清理

### 3.4 错误恢复（retry / fallback）

**现状**：**零 retry**。工具失败：
- `ok=false, error="..."` 走正常 result 路径写回 messages
- LLM 看到失败可能自己改参再调（视模型水平），也可能直接放弃改答其他

**典型失败场景统计**（来自最近 7 天 write_audit.log + plugin.log 粗采）：
| 失败类型 | 频次 | LLM 自恢复率（粗估） |
|---|---|---|
| 参数 schema 拼错（如 size 给字符串 "abc"） | 高 | ~80%（看到 error 后改） |
| 网络/HTTP 5xx（provider 偶发） | 中 | 0%（直接 error 给用户） |
| 工具内部 SDK 调用瞬时失败（如 DbgEval 刚启动时未就绪） | 中 | ~40% |
| confirm 被用户拒绝 | 低 | 视情况，常常直接放弃 |

**结论**：⚠️ provider HTTP 失败不重试是明显短板；SDK 瞬时失败重试一次能解约一半

### 3.5 RAG / case memory 集成

**现状**：
- ✅ RAG 后端完整：sqlite + sqlite-vec，按 SHA256 分库，文档/note/case 三类条目，embedding 走 GitHub Models PAT
- ✅ 工具 `rag_search(query, k?)` 暴露给 LLM
- ❌ **agent loop 完全不预注入**——LLM 必须在 system prompt 自己写"先调 rag_search"才会用；当前 15 个预设里只有 2 个 case-driven 预设这么做了

**对比**：Cursor 自动注入"相关代码片段"；Claude Code 把 codebase index 自动放 system

### 3.6 二级 agent（plan / critic / sub-agent）

**现状**：**无**。单 LLM 单循环到底。

**对比**：
- Claude Code 有内置 `Task` 工具可派生 sub-agent（带独立上下文，结果返主）
- AutoGen / LangGraph 框架支持 planner + executor + critic 三 agent 协作

---

## 4. Tool Surface 评估（横向对比）

| 维度 | x64dbg-ai-plugin | Claude Code | Cursor agent |
|---|---|---|---|
| 工具数 | 74 | ~15 内置 + MCP 扩展 | ~10 |
| 工具粒度 | 细（read_memory / disasm_at / set_breakpoint 分开） | 中（Read/Edit/Bash 通用） | 中 |
| 工具 schema 质量 | 高（含中文 description + zh-CN tag） | 高 | 高 |
| 写工具护栏 | **5s confirm + audit + 二级护栏 K-30/K-32** | edit 前 read 强制 | edit 前 read 强制 |
| 工具结果截断 | 64 KB 硬截断 + truncated 标识 | 类似 | 类似 |
| 可观测性 | write_audit.log + plugin.log + UI 卡片三层 | 仅 UI | 仅 UI |

**结论**：tool surface **不输商业产品**，护栏体系甚至更系统化（双层 ACL + 详细 audit）；差距全在编排能力。

---

## 5. 实战表现（用过的 task 类型）

| Task | 表现 | 主要瓶颈 |
|---|---|---|
| 单函数反汇编 + 解释 | ⭐⭐⭐⭐⭐ 优秀 | 无 |
| 中等深度 crackme（找算法 + patch） | ⭐⭐⭐⭐ 良好 | 串行 tool 慢；偶尔重复调 |
| Anti-debug 诊断（PEB/SEH/handle/window 综合） | ⭐⭐⭐ 中等 | 工具调多了上下文涨；无并行明显慢 |
| 完整脱壳流程（多阶段 OEP 寻找 + dump + IAT 修复） | ⭐⭐ 偏弱 | 需要 plan 拆解；当前没有 dump 工具闭环（Scylla 未暴露 SDK） |
| 长会话（>30 轮） | ⭐⭐ 偏弱 | 上下文压力；LLM 开始遗忘最初任务 |

---

## 6. 改进项 ROI 排序（执行建议）

按 **(质量提升 × 频次) ÷ 实现成本** 排序。

### #1 失败工具自动 retry（成本：S，收益：M）

**实现**：`agent_loop.cpp::dispatch` 外层加 retry decorator。
```cpp
// 伪代码
for (int attempt = 0; attempt < kMaxRetry; ++attempt) {
    auto r = dispatchOnce(name, args, ctx);
    if (r.ok || !isRetryable(r.error)) return r;
    if (attempt < kMaxRetry - 1) {
        spdlog::warn("[agent] retry tool={} attempt={}", name, attempt + 1);
        std::this_thread::sleep_for(100ms * (1 << attempt));  // 100/200/400ms
    }
}
```

**关键决策**：
- `kMaxRetry = 3`；只对 retryable 错误重试（网络 / SDK busy / 短暂 DbgEval 失败）
- `isRetryable` 白名单识别：error 文本含 `timeout` / `temporarily unavailable` / `DbgIsDebugging returned false` 等
- 写类工具**不重试**（confirm 已通过 + 业务失败大都是逻辑错，重试无意义且改变状态）
- audit 日志区分 `attempt` 字段
- 单 tool 总时长仍 ≤ 单 turn 软超时（见 #3 的延伸）

**预计代码量**：≤ 50 行

### #2 RAG 在 agent 路径下自动检索注入（成本：M，收益：H）

**实现**：`agent_loop.cpp::run` 入口、user 最新消息之后、第一次 streamChat 之前，做一次自动检索：
```cpp
if (preset.autoRagInject && !messages.lastUserText().empty()) {
    auto hits = ragSearch(messages.lastUserText(), /*k=*/3);
    if (!hits.empty()) {
        messages.push({role: "system",
            content: "## 相关历史经验\n" + formatHits(hits) + "\n请优先参考"});
    }
}
```

**关键决策**：
- 预设新增 `autoRagInject: bool`（默认 false 保后向）
- 注入位置选 system 末尾不污染 user/assistant 流；turn 间不重复注入（只首轮）
- k=3 控制 token 涨幅 ≤ 1.5K
- hit score 阈值（如 cosine ≥ 0.7）才注入，避免硬塞低相关
- 出厂预设默认开启 `autoRagInject` 的：`analyze-function` / `crackme-solver` / `unpack-helper`

**预计代码量**：约 100 行（含预设字段、检索调用、format util）

### #3 上下文压缩（成本：M-L，收益：H）

**实现**：当 `messages` 估算 tokens > 阈值（如 40K）时，把最早的若干轮 `(assistant tool_call, tool result)` 折叠成一条 system summary：
```
"已折叠 12 轮工具历史摘要：
- read_memory(0x401000, 64) → 函数序言 push rbp/sub rsp/...
- disasm_at(0x401050, 20) → 主循环含 XOR 解密
- ... (10 项省略)
共调用工具 12 次，关键发现：XOR key=0xDE 在 0x401080；进入解密循环前栈帧 rsp=...
"
```

**关键决策**：
- 阈值选 LLM context 60-70%（DeepSeek 64K → 40-45K 触发）
- 折叠最早的 70%，保留最近 30% 原样（recency bias 友好）
- 摘要让 LLM 自己生成（再调一次 streamChat 走 "summarize these tool histories" 子任务）；不在本地启发式总结
- 折叠后保留原 tool_call_id 占位（避免后续 LLM 引用不到）
- audit log 记录折叠事件（fold_event with from_iter/to_iter/saved_tokens）

**预计代码量**：约 200 行（含 token 估算、summarization sub-call、message 替换）

### #4 只读工具并行执行（成本：M，收益：M）

**实现**：`agent_loop.cpp::dispatch` 改成「拓扑」执行——同一轮 `tool_calls` 中，所有 ToolCategory == Read 的工具用 `std::async` / Qt thread pool 并行调，DbgControl/Write 仍串行。

**关键决策**：
- 只对 Read **并行**；Write 串行（避免 confirm 对话框重叠 + audit 顺序保护）
- 并发上限 = 4（避免一次提 10 个 read_memory 拖死 SDK）
- 结果按原 tool_call 顺序写回 messages（LLM 仍然按 schema 顺序看到）
- 异常处理：单个并行 task 失败不影响其他

**预计代码量**：约 150 行（注意 ToolContext 线程安全审计）

### #5 Plan-Execute 两阶段（成本：L，收益：H 但只对复杂 task）

**实现**：新增 `plan_then_execute` 预设模式：
1. **Planning turn**：LLM 用 reduced tool set（只有 `propose_plan(steps: [...])` 一个工具）拆解 task
2. **Execution turns**：按 plan 逐步执行，每步可走完整 ReAct
3. **Replan**：某步失败 / 偏离时可调 `revise_plan(...)` 改后续步骤

**关键决策**：
- 不强制所有预设走 plan-execute；只 opt-in（适合脱壳 / 漏洞挖掘类复杂 task）
- Plan 数据结构存 `PresetSession.plan: [Step]`，UI 顶栏显示 progress
- 太简单 task（如 "看 0x401000 的反汇编"）触发 plan 反而绕弯——LLM 自己决定是否启用

**预计代码量**：约 500-800 行（新增 plan 数据结构、UI、sub-agent 调度）

---

## 7. 不建议做的反向项

| 项 | 不建议原因 |
|---|---|
| 让 agent 自动选预设 | LLM 选错预设比用户选错代价大；用户多选一下不是问题 |
| Case memory 写入也自动化 | 自动写 case 会产生大量低质条目污染 RAG；保留用户手动「沉淀本次会话」按钮 |
| 多 LLM 并行（DeepSeek + Copilot 同 task） | 成本翻倍但融合策略难定；目前单 provider 已够用 |
| Streaming 工具结果 | 工具结果都是离散 JSON，没有 stream 必要 |

---

## 8. 下批迭代建议路线

按 ROI 排序逐项做，K-33 ~ K-37 占位：

- **K-33**：失败工具自动 retry（#1）—— 1 个 PR，1-2 天
- **K-34**：RAG agent 路径自动注入（#2）—— 1 个 PR，2-3 天
- **K-35**：上下文压缩（#3）—— 1 个 PR，3-5 天
- **K-36**：只读工具并行（#4）—— 1 个 PR，3-4 天
- **K-37**：Plan-Execute 模式（#5）—— 多 PR，1-2 周

预期完成 K-33 + K-34 + K-35 后，agent 等级从「L2 末/L3 初」推进到「L3 末」；做完 K-37 进入「L4 初」。

---

## 9. 变更历史

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-05-28 | 1.0 | 首次评估，基于 K-32 完成态代码 |
