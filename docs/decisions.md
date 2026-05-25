# 决策日志（ADR-lite）

记录开发过程中**带选项权衡**的关键决策。区别于 `development-log.md`（按里程碑记"做了什么"），本文件按时间倒序记"为什么这么选 / 否决了什么"。

格式：每条决策一段卡片，结构固定 4 段：
- **背景**：触发决策的问题或诉求
- **候选方案**：列出讨论过的 A/B/C…（被否决的也记）
- **选定**：最终选哪个 + 一句理由
- **代价 / 复盘**：放弃了什么、未来可能后悔的点

仅记**有 2+ 个候选并发生过权衡**的决策；纯实施细节不记（去 dev-log）。

---

## 2026-05-25 · G-2 显示位置选 agentStatusLabel_ 末尾拼接

**背景**：G-2 prompt cache 观测需要在 UI 暴露 hit ratio，要决定显示位置。

**候选方案**：
- **A. 新增独立 cache label**：顶栏多一个 `QLabel`，专门显示 cache 状态
- **B. 拼在现有 `agentStatusLabel_` 末尾**：保留预设名前缀，末尾追加 `· input=N cache=N%`
- **C. 只写日志，不上 UI**：纯调试观测，等需要时再做 UI

**选定**：B。
- 顶栏宽度有限，多 label 会挤压
- 预设名 + cache 状态属同维度上下文，并列读
- 详细数据走 ToolTip 不占空间

**代价**：label 字符串拼接逻辑变复杂（新增 `lastCacheStatus_` 成员 + `setActivePreset` 重画）；ToolTip 信息只在悬停才看到，新用户可能发现不了；长预设名 + 长数字可能挤压旁边按钮（K-26）。

---

## 2026-05-25 · G-2 commit 策略——独立 commit 不打 tag

**背景**：G-2 完成后要不要打 milestone tag。

**候选方案**：
- **A. 打 `g-2-done` tag**：标记单点改进完成
- **B. 独立 commit 但不打 tag**：留到下次 milestone 时统一打
- **C. 合并进下一个 S 阶段一起 commit**：减少 commit 数

**选定**：B。
- G-2 是单点改进非里程碑，不值得占用 tag 命名空间
- 独立 commit 利于回溯（被发现 cache 解析有 bug 时易定位）
- 合并会让 commit message 失焦

**代价**：暂时无 tag 锚点；若有 bug 排查需靠 commit hash `5b42e52`。

---

## 2026-05-25 · 不为 sample-triage 做 sub-agent 框架

**背景**：方案 C 实施时考虑：让 sample-triage 作为 unpack-helper / malware-triage / anti-anti-debug 的子 agent 自动跑 PHASE 0 预检。

**候选方案**：
- **A. 上 sub-agent 框架**：AgentLoop 加 child-agent 机制，让 PHASE 0 是独立 LLM 上下文
- **B. inline PHASE 0**：每个场景预设的 systemPrompt 头部嵌入 2-3 call 的自检段，共享同一对话上下文
- **C. 不做预检**：保持原状，用户自己选对预设

**选定**：B（inline PHASE 0）+ 同时保留 sample-triage 作独立完整预检预设。
- A 改动量巨大（违反"最小改动"原则）
- B 共享 prompt cache + 单轮对话体验更好
- C 与用户反馈的"前提硬编码导致空转"矛盾

**代价**：每个场景预设 systemPrompt 长度增加 ~30%（PHASE 0 文本）；PHASE 0 与场景核心 prompt 共享 token budget，复杂样本可能压缩主流程余量（K-23）。

**复盘触发**：若未来出现"自动选预设链式跑"诉求（明显需要父子 agent 切换上下文），重审。已记 N-06。

---

## 2026-05-25 · sample-triage 归 exploration 组而非新开 triage 组

**背景**：新增 sample-triage 预设要归类。

**候选方案**：
- **A. 新开 `triage` 组**：和 exploration / cracking / tracing / scenarios 平级
- **B. 归 exploration 组 + 加 `triage` tag**：用标签区分而非分组

**选定**：B。
- 单预设单开一组冗余
- 现有 6 个 exploration 预设语义相近（都是"先看看再说"）
- tag 系统已存在，正好用

**代价**：未来若 triage 类预设增至 3+ 个，要回头重组；exploration 组现在 6 个略偏多。

---

## 2026-05-25 · 保留 malware-triage 不与 sample-triage 合并

**背景**：两预设名字接近，是否合并。

**候选方案**：
- **A. 合并为单一 triage 预设**：减少出厂预设数
- **B. 保留两个，分工不同**

**选定**：B。
- sample-triage = **形态预判**（加壳/反调试/EP/IAT）4 维度结构判断，5 calls 上限
- malware-triage = **行为分诊**（句柄/窗口/TCP/SEH/RWX 私有内存）深度行为扫描，maxIter=25
- 二者目标不同：sample-triage 帮选下一个预设，malware-triage 是终极行为扫描

**代价**：用户面对两个含 "triage" 的预设可能选错；通过 description 区分（"形态预判 / 行为分诊"）。

---

## 2026-05-25 · S9 G-9 工具描述中文化只做 A 档（UI only）

**背景**：S9 期间用户提出工具描述全量中文化诉求。

**候选方案**：
- **A. UI only**：UI 显示中文，LLM 仍读英文 description（加 `descriptionZh()` 方法）
- **B. 全量中文化**：description() 直接返中文，LLM 也读中文
- **C. 不做**：保持纯英文

**选定**：A。
- 保护 prompt cache（system prompt + tools schema 英文一致）
- 中文 token 占用 ~+30%，B 方案直接抬升每轮成本
- B 在 G-2 验证 cache 命中率前是盲改

**代价**：维护两份 description（英文 + 中文），63 工具 = 126 处文案；新增工具时容易忘加 `descriptionZh()`（已加编译期校验：override 数量配对）。

**复盘触发**：G-2 runtime 数据出来后，重审 B 档（userTemplate 中文化）是否值得。

---

## 2026-05-25 · S9 G-10 工具勾选 UI 用 QTreeWidget 不用分组 ListWidget

**背景**：S8 后工具 63 个，原 PresetEditor 单 QListWidget 平铺过载。

**候选方案**：
- **A. QTreeWidget 三态勾选**：group 节点 + 子工具，整组勾/反勾/部分勾
- **B. QListWidget 加 group 分隔行**：保留 list 结构，用 disable 行做视觉分组
- **C. 多个 QListWidget 横向 split**：每组一个独立 list

**选定**：A。
- 整组勾选语义最自然（三态原生支持）
- 搜索过滤时层级保留
- Qt 原生组件，无需自绘

**代价**：QTreeWidget API 比 QListWidget 啰嗦；三态自绘 indicator 适配 dark 主题需在 QSS 写多组 image 路径。

---

## 2026-05-24 · S8 不暴露 stack_pop

**背景**：S8 加注入/栈工具集，要决定栈操作工具粒度。

**候选方案**：
- **A. 提供 stack_pop**：完整的 push/pop/peek 三件套
- **B. 仅 stack_push + stack_peek**：需弹出让 LLM 用 peek + set_register 显式做

**选定**：B。
- 真弹出破坏 ESP 一致性，反向工程几乎用不到（用 peek + 偏移已够）
- 显式两步让 agent 思考"我到底要不要改栈"，减少误操作

**代价**：弹+恢复路径变两步；agent prompt 需教这个模式。

---

## 2026-05-24 · S8 anti-debug-bypass 合并入 anti-anti-debug

**背景**：S8 原计划新增 `anti-debug-bypass` 与 `anti-anti-debug` 双预设。

**候选方案**：
- **A. 拆两预设**：诊断（被动）与中和（主动）分开
- **B. 合并为增强版 anti-anti-debug**：systemPrompt 改"先 passive 后 active"

**选定**：B。
- 语义大量重叠，拆只是切阶段不切工具
- 单预设的两阶段 workflow 比双预设跳转上下文连贯

**代价**：单预设白名单变大（30+ 工具）；用户无法只跑诊断阶段（但 sample-triage 后来补上了这个生态位）。

---

## 2026-05-24 · S7 get_cfg 只输出 Mermaid 不带结构化 JSON

**背景**：CFG 工具输出格式选型。

**候选方案**：
- **A. 同时返 nodes/edges JSON + mermaid 字符串**：结构化数据 + 渲染源
- **B. 只返 mermaid**：单一字符串

**选定**：B。
- Mermaid 节点定义本身已把 start/end/icount/terminal/icall 编进 label，能从字符串提取
- A 方案 token 翻倍

**代价**：LLM 想结构化需从 mermaid 文本解析；UI 渲染端有 mermaid 围栏即可。

---

## 2026-05-24 · S7 set_conditional_bp 失败即返不回滚

**背景**：set_conditional_bp 多字段写入，部分字段失败如何处理。

**候选方案**：
- **A. 任一字段失败回滚已写**：事务语义
- **B. 失败立即返，附 `applied` 数组**：告知 LLM 哪些已生效

**选定**：B。
- BP_REF 无事务支持，回滚需先读旧值（代码翻倍）
- `applied` 字段给 LLM 足够信息自我修复

**代价**：留下部分写入状态；agent 可能需第二轮清理。

---

## 2026-05-23 · S6 label / comment 删除用 `text=""` 而非独立工具

**背景**：set_label / set_comment 工具如何支持"删除"语义。

**候选方案**：
- **A. 另开 delete_label / delete_comment**：4 个工具
- **B. set_* 用空串触发删除**：2 个工具

**选定**：B。
- 减少同义工具决策噪声（LLM 不用纠结用 set 还是 delete）
- 工具数少 → tools schema 占 token 少

**代价**：LLM 需理解"空串=删除"语义；description 必须明确写。

---

## 2026-05-23 · S6 run_continue 默认 fire-and-forget

**背景**：run_continue 工具阻塞模型选型。

**候选方案**：
- **A. 默认同步等 Paused**：调完即知结果
- **B. 默认 fire-and-forget + 可选 wait_for_stop=true**

**选定**：B。
- 长跑场景（等下个断点可能几分钟）不浪费 agent loop iteration
- LLM 显式用 wait_for_event 更精确

**代价**：LLM prompt 需教"调 run_continue 后通常要 wait_for_event"。

---

## 2026-05-22 · S3 写工具 5s 倒计时强制 confirm

**背景**：S3 引入写工具，需要确认机制保护用户。

**候选方案**：
- **A. 始终模态阻塞**：用户点 OK 才执行，无超时
- **B. 5s 倒计时 + ESC/Enter 默认拒绝**：超时不点视为拒绝
- **C. preset.json 加 `autoApprove` 白名单**：跳过 confirm
- **D. 静默执行**：纯审计无确认

**选定**：B。A 与 C 都不做。
- A 长会话累积疲劳，用户可能盲点 OK
- 5s 强制看一眼，又不至于卡死流程
- C 留待用户明确反馈后再做（避免过早开放风险面）

**代价**：所有写工具不可绕过 5s；高频写场景（如批量 patch_memory）变慢；用户至今未反馈想要 autoApprove。

---

## 2026-05-22 · S3 ToolPolicy 三档（Read / DbgControl / Write）

**背景**：S3 引入工具策略分类。

**候选方案**：
- **A. 两档**：Read / Write
- **B. 三档**：Read / DbgControl / Write

**选定**：B。
- wait_for_event / step_* / run_* 是"控制类"：要 audit 但不需要 confirm（用户主动让 agent 走的）
- 归入 Write 会被 5s 倒计时拖垮交互节奏
- 归入 Read 又失去审计可追溯性

**代价**：分类逻辑稍复杂；新工具加时需正确分档（已有几个工具改过档）。

---

## 2026-05-22 · S3 审计日志独立 logger

**背景**：S3 引入 write_audit 需求。

**候选方案**：
- **A. plugin.log 加 tag**：所有日志一起
- **B. 独立 spdlog logger + write_audit.log，pattern 裸 `%v`**：纯 JSON 单行

**选定**：B。
- 审计日志要 grep / jq 友好（不要时间戳前缀污染）
- 出问题时单独保留 / 单独清理
- spdlog 多 logger 成本低

**代价**：多一个日志文件管理；总磁盘占用稍增。

---

## 2026-05-21 · D-03 analyze-function 加 v4 evidence rule（schema 4）

**背景**：fx_log1.txt 暴露 AI 凭空推断调用关系、未代入入参演算。

**候选方案**：
- **A. 保留旧 "reason from concrete bytes" 弱措辞**：靠 LLM 自觉
- **B. 加两条硬规则 + bump schema v4**：EVIDENCE RULE + CONCRETE INPUT RULE

**选定**：B。
- 弱措辞已证明不够
- bump schema 触发用户库迁移，确保所有 analyze-function/who-calls-here/string-api-context 升级到位

**代价**：systemPrompt 变长占 context；schema 升级需走 preset 覆盖路径；后续每次新增类似预设都要复盘是否套这两条规则。

---

## 2026-05-20 · M4.6c ChatView 用 QScrollArea + VBox 容器

**背景**：M4 Agent UI 要嵌 ToolCallCard / ReasoningBlock 等任意 QWidget。

**候选方案**：
- **A. QTextDocument + 自定义 ObjectInterface**：富文本嵌入 widget
- **B. QScrollArea + VBox 容器**：每条独立 QWidget

**选定**：B。
- A 在 Qt 上嵌 QWidget 复杂、流式更新困难
- B 摆脱 setHtml 卡顿，每个 widget 自管理生命周期

**代价**：流式只能用 QLabel.setText（富文本支持有限）；样式由 QSS 控制不是 HTML。

---

## 2026-05-19 · M4.2 工具粒度——细粒度只读工具 LLM 自组合

**背景**：M4 Agent 工具设计粒度。

**候选方案**：
- **A. 粗封装**：一个 `analyze_function(addr)` 大工具内部完成 10 步
- **B. 细粒度**：12 个只读工具，LLM 自己决定调哪个

**选定**：B。
- 细粒度可审计（每步可看）
- 用户可控资源（按预设白名单）
- 写工具可单独管控

**代价**：LLM 需多轮调用 → 总 token 消耗高、首响应慢；通过 prompt cache（G-2）+ 预设白名单缓解。

---

## 2026-05-19 · M4.1 Agent 协议自实现不走 MCP

**背景**：M4 引入工具调用，需选协议。

**候选方案**：
- **A. 接入 MCP SDK**：生态兼容
- **B. 自实现 ToolRegistry + 兼容 OpenAI function calling**

**选定**：B。
- MCP 需 stdio/SSE 子进程，单 dll 插件架构不适配
- 不需要跨进程隔离（工具直接调 x64dbg SDK）
- OpenAI function calling 已是事实标准

**代价**：放弃 MCP 生态兼容；外部 MCP 工具无法直接接入（需自己包装）。

---

## 2026-05-18 · M3.6 跨 DB 会话浏览（不改 ProjectId 算法）

**背景**：EXE 重编译导致 SHA 变化，老会话历史"失联"。

**候选方案**：
- **A. 让用户手动改 DB 文件名**：纯外部操作
- **B. 改 ProjectId 算法（用导出表 hash）+ 老库迁移**：根本解决
- **C. 加 UI 跨 DB 浏览导入**：把消息从老库拷到新库
- **D. 配置别名映射**：sha → 别名

**选定**：C。
- B 风险高（迁移失败损毁老库）
- A 用户不友好
- D 配置膨胀
- C 保留 SHA 隔离 + 只拷 messages 不带 chunks（不污染当前 RAG）

**代价**：用户需手动选择导入；同一项目多版本会拉多个 DB（已有 0 KB 清理脚本配合）。

---

## 2026-05-15 · M3.5 改 CallStack 反向采样（放弃顺向 trace）

**背景**：调用链追溯遇到 ws2_32.send syscall 跟丢。

**候选方案**：
- **A. 解析 .trace64 文件**：x64dbg trace 日志格式
- **B. 主动 trace + cbTraceExecute 实时构图**：每步监听
- **C. 叶子 API 命中时反向读线程栈**：被动采样

**选定**：先 B（失败）后 C。
- B 撞 syscall 边界跟不进系统层
- C 利用 RtlVirtualUnwind 反向走，绕过 syscall 限制
- A 数据格式不稳定且无实时性

**代价**：只看到 caller chain 不看 callee；适用于"谁调了我"不适用于"我调了谁"。

---

## 2026-05-14 · M3.4 ChatView 闪烁修复用 QTextCursor 增量追加

**背景**：流式聊天每帧 setHtml 全量重排导致闪烁。

**候选方案**：
- **A. QTextCursor 末尾增量追加 + 16ms 节流**：保留 QTextEdit
- **B. 换 QWebEngineView**：完全前端化
- **C. 改纯 div 仍走 setHtml**：DOM 优化

**选定**：A。
- B 太重（拖几十 MB 依赖）
- C 治标不治本
- A 性能足够，且为后续 M4.6c 嵌 widget 留路

**代价**：必须严格守护 streamCursor 生命周期（任何 setHtml 前清空）；外部 rerender 路径都要先 finalize。

---

## 模板：未来决策卡片

```markdown
## YYYY-MM-DD · 决策标题（动词开头）

**背景**：…

**候选方案**：
- **A. …**：…
- **B. …**：…

**选定**：…。一句话理由。

**代价**：…

**复盘触发**（可选）：什么条件下回来重审这个决策
```
