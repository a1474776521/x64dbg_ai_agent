# 决策日志（ADR-lite）

记录开发过程中**带选项权衡**的关键决策。区别于 `development-log.md`（按里程碑记"做了什么"），本文件按时间倒序记"为什么这么选 / 否决了什么"。

格式：每条决策一段卡片，结构固定 4 段：
- **背景**：触发决策的问题或诉求
- **候选方案**：列出讨论过的 A/B/C…（被否决的也记）
- **选定**：最终选哪个 + 一句理由
- **代价 / 复盘**：放弃了什么、未来可能后悔的点

仅记**有 2+ 个候选并发生过权衡**的决策；纯实施细节不记（去 dev-log）。

---

## 2026-05-28 · K-32：白/黑名单抽公共模块 + 独立只读 UI（否决「主表加列」与「UI 可编辑」）

**背景**：K-30 引入的 `kSysMods` / `kHotApis` / `classifyBpAddr` 在 `debug_write_tools.cpp` 和 `advanced_bp_tools.cpp` 各有一份完全相同的副本；未来加第三个断点类工具会出现第三份。同时用户问"我怎么知道 `run_dbg_command` 允许什么命令" / "怎么扩白名单"找不到地方，「已注册工具一览」也没暴露 ACL 信息。

**候选方案**：
- **A. 复制现状不抽公共模块**：每加一个断点工具就 copy-paste；UI 上在主表加 1 列「安全护栏」简短文字
- **B. 抽 `bp_safety.h` 公共模块 + 主表加列**：去重；UI 在工具表加一列"Safety"，每行写 "whitelist" / "K-30 blacklist" / "—"
- **C. 抽公共模块 + 独立只读对话框 + 工具详情交叉引用**（最终）：去重；新建 `SafetyBrowserDialog` 4 tab；主表只在底部加一个按钮，三个受影响工具详情窗加跳转链接
- **D. 公共模块 + UI 可编辑（白名单 + 黑名单都能改）**：把配置面板做进 UI

**选定**：C。
- A 拒绝：副本必然漂移（K-30 已经在两份代码里都加了 `LdrLoadDll`，下次加 K-33 再加一个，必然漏一处）
- B 拒绝：74 个工具里只有 3 个受护栏影响，加列对 71 个工具是 N/A 噪声；列宽窄了也写不下"K-30 blacklist (sys mod + hot api)"这种语义
- D 拒绝：黑名单（K-30 sys/hot）是「防卡死」硬护栏，UI 可改 = LLM 也能通过 prompt 注入诱导用户改；白名单虽然可追加但要走文件编辑 + 重启，给一个心理 gate；UI 只读是有意保留的"摩擦"

**代价 / 复盘**：
- `dbgCmdWhitelist()` 合并集走 `static once_flag`，**修改 config 必须重启插件**，UI 已显式提示；如果用户没看提示改完不重启，会以为没生效——可接受
- 黑名单未来若有合理覆盖场景（如自研壳的 user32 钩子）需新增 `extra_safe_apis` 字段（白名单覆盖黑名单），届时同走 config + 重启
- "在详情窗加跳转链接"模式可复用：将来其他工具（如 RAG / preset）也有需要展示规则的场景，可同模式新建 `*BrowserDialog`

---

## 2026-05-28 · K-31：会话生命周期五件套独立工具（否决「合一个 manage_debug」与「让 LLM 走 run_dbg_command」）

**背景**：LLM 不能启动 / 重启 / 附加 / 脱离 / 结束调试会话；调试中工具大都假设"已经在调试"，但没有进入这个状态的入口。

**候选方案**：
- **A. 让 LLM 用 `run_dbg_command("init <file>")` / `run_dbg_command("attach 0x...")` 通过命令逃生口完成**：不加新工具
- **B. 合并成一个 `manage_debug(action, ...)` 工具**：枚举 `start | attach | detach | restart | stop`
- **C. 拆成 5 个独立工具**（最终）：`start_debug` / `attach_debug` / `detach_debug` / `restart_debug` / `stop_debug`

**选定**：C。
- A 拒绝：必须给 `init` / `attach` 进白名单，但这两个命令的参数（文件路径含空格、PID 进制混淆）极易写错，5s confirm 也很难看出"这段命令到底是要 attach 哪个 PID"；独立工具的 args JSON 在 confirm 对话框里清晰得多
- B 拒绝：5 个 action 的参数 schema 各不相同（start 要 file/args/cwd，attach 要 pid，其他无参），合一个会把 schema 写成 union，LLM 容易漏参或乱填；分散成 5 个工具反而每个 schema 极简
- C 接受：工具数 +5 看起来多，但每个 schema 干净；description 各自独立可写清楚 detach vs stop 的语义差别（不杀 vs 杀进程）；统一走 Write + 5s confirm + audit

**代价 / 复盘**：
- 工具总数 69→74；接近 LLM context 中 tool list 的甜区上限（>100 后 GPT/DeepSeek 选错率开始上升）
- `start_debug` 用 `DbgCmdExec("init ...")` fire-and-forget，不等启动完成；LLM 需要后接 `wait_for_event(["DebugStarted"])` 自己同步——已在 description 写明
- `restart_debug` 依赖 x64dbg 内部状态保留命令行 / cwd，没在工具参数里再传一遍；首次启动用 `start_debug` 走 init，restart 不能重设参数（要重设就再调 `start_debug`）

---

## 2026-05-28 · K-30：系统 API 高频符号黑名单 = 模块∧符号（否决「只看模块」与「只看符号」）

**背景**：用户 LLM 自动调 `set_breakpoint("kernel32.LoadLibraryW")`，断在 LoadLibraryW 入口；进程启动加载阶段 LoadLibrary 每秒被调几十次，x64dbg 每次断点把整个进程暂停几百毫秒，**全局键鼠 hook 被频繁吃掉 → 整个桌面卡死**，只能 hard reset。

**候选方案**：
- **A. 拒绝所有系统模块下断**（按模块黑名单）：只要 `ntdll/kernel32/...` 都不让下
- **B. 拒绝所有高频 API 下断**（按符号黑名单）：只要符号在 LoadLibrary/HeapAlloc/... 内就拒
- **C. 模块 ∧ 符号 双条件**（最终）：必须**系统模块**且**热 API**才拒绝
- **D. 不拒，只警告**：description 加红字，LLM 自己决定

**选定**：C。
- A 拒绝：用户合法用例「在 `ntdll.RtlInitUnicodeString` 上看参数」会被误杀（这 API 频率不算极高）
- B 拒绝：用户自己实现的 `LoadLibrary` wrapper（如某些壳/loader）符号正好叫 `LoadLibrary`，但所在模块不是系统模块——这种应该让下断
- D 拒绝：LLM 在自动模式下不会读 description 警告（已实测过 K-30 修复前的版本 LLM 完全无视"may freeze"提示）
- C 双条件交集最小化误杀，"系统模块的高频 API 入口"几乎 100% 是危险点

**代价 / 复盘**：
- 维护两份名单成本：系统模块 24 项相对稳定；热 API 60 项可能漏（如 Win11 新增的 API、用户自定义工作流要看的 `Sleep` / `QueryPerformanceCounter` 也算热）
- 拒绝时 description 给两条出路：`set_conditional_bp(condition=...)` 让内核过滤 / 调用方下断；亲测 LLM 看到后能正确改写
- 仅对硬断 `type=execute` 拦截，write/access 类不拦（这些不会反复命中）
- K-32 已把名单抽公共模块，未来加项只改一处

---

## 2026-05-28 · K-29：`search_pattern` 模块解算用 `ModSizeFromAddr` 替 `DbgEval`（否决「让 LLM 先 get_module_info」）

**背景**：`search_pattern(pattern, module="kernel32.dll")` 长期报错 `"failed to resolve module range"`；定位原因是内部用 `DbgEval("kernel32:start")` / `DbgEval("kernel32:end")` 解算模块范围，但 x64dbg 表达式引擎对带 `.dll` 的模块名有时返回 0；同时 `agent_loop` 在工具 ok=false 时只 INFO 一行，LLM 看不到具体 error 字符串。

**候选方案**：
- **A. 文档化要求**：让用户/LLM 自己先 `get_module_info` 拿到 base/size，再走 `search_pattern(start=base, size=size)`，不修工具
- **B. `search_pattern` 内部改用 `ModBaseFromName` + `ModSizeFromAddr`**（最终）：直接走稳定 SDK
- **C. 改用 `DbgEval` 但带 fallback**：先 `kernel32:start`，再 `kernel32.dll:start`，再 `mod.base(kernel32)` 一路尝试

**选定**：B + 同时修 agent_loop 让失败工具 WARN 出 error 文本。
- A 拒绝：把工具的内部 brittleness 推给 LLM 不可接受，agent 调用路径越长越易跑偏
- C 拒绝：多 fallback 仍可能在某些版本 x64dbg 全失败；SDK 函数是契约 API 比表达式稳
- B 一步到位：`ModBaseFromName` 接 `"kernel32.dll"` / `"kernel32"` 都能解；`ModSizeFromAddr(base)` 直接拿尺寸

**代价 / 复盘**：
- `agent_loop` 加 WARN 是顺手活：之前只 INFO `"tool returned: ok=false"`，现在 WARN 带 `tool=... error=...`，复盘 .log 时省事很多
- 暴露了一个隐含规范："工具失败 = WARN 级别"——后续新工具如果走自己的 result 构造跳过中央 dispatch，要注意手动 WARN

---

## 2026-05-28 · K-28：`patch_memory` 走 `MemPatch`，新增 `patch_file` 工具（否决「`patch_memory` 内部自动 patch_file」）

**背景**：`patch_memory` 之前直接走 `DbgMemWrite`，写完字节就完事——但 x64dbg 内部 patch 系统（`Patches` tab）一无所知 → 不可在 UI 看到、不可撤销（`restore_patch` 找不到记录）、不可导出补丁文件（`patch_file` 命令找不到补丁）。

**候选方案**：
- **A. `patch_memory` 仍走 `DbgMemWrite`，再加一个 `register_patch(addr, original, new)` 工具让 LLM 显式登记**：拆两步
- **B. `patch_memory` 内部改走 `DbgFunctions()->MemPatch`，自动登记**（最终的一半）：单工具内透明完成
- **C. `patch_memory` 内部 `MemPatch` + 自动调 `PatchFile` 立即落盘**：彻底自动化
- **D. B + 显式独立工具 `patch_file()` 让 LLM 自己决定何时落盘**（最终）

**选定**：D = B + 新增 `patch_file`。
- A 拒绝：LLM 容易忘记调 register_patch，留下 ghost write
- C 拒绝：落盘是不可撤销操作，应该 LLM 显式确认；自动落盘 + 5s confirm 会让单次 patch_memory 弹两次 confirm 影响体验
- D 平衡：patch_memory 永远登记到 Patches tab 让 restore 可用，但落盘是独立 step 走独立 confirm

**代价 / 复盘**：
- 工具数 68→69（K-28 之后）；description 在 `patch_memory` 加一段说明"已自动加入 Patches，可用 restore_patch 撤销，落盘请用 patch_file"
- 旧版本残留的 ghost write（K-28 前用 DbgMemWrite 改过的字节）restore_patch 仍找不到——可接受，老 session 不追溯

---

## 2026-05-26 · 中文路径根治：边界转码 + 内部 UTF-8（否决全链 wchar）

**背景**：x64dbg SDK 的 `PLUG_CB_INITDEBUG.szFileName` 是 ACP（中文系统=GBK），MSVC 的 `std::filesystem::path(std::string)` 把入参当 ACP 解码，导致中文路径全链失败：`fs::exists` 返回 false → `SessionStore` 永远不开 → UI 显示"未在调试"；历史浏览器读 db meta 也乱码。

**候选方案**：
- **A. 全链改 std::wstring / std::filesystem::path 内部统一 wchar**：把所有 `std::string` 路径字段改 `std::wstring`，SDK 边界 ACP→wchar，sqlite 走 `sqlite3_open16`，spdlog 开 `SPDLOG_WCHAR_FILENAMES`
- **B. 边界转码 + 内部 UTF-8**：插件内部一律 `std::string`（UTF-8）；与 SDK / Win32 / `std::filesystem` 交互的"边界"做 `ansiToUtf8` / `utf8ToWide`；`fs::path` 用 `wstring` 构造规避 MSVC 把 string 当 ACP 的坑；sqlite 用 UTF-8 的 `sqlite3_open`（SQLite 官方就接 UTF-8）
- **C. 折中：只修 ProjectContext 一处**：在 `onDebugStart` 入口 ANSI→UTF-8，下游不动

**选定**：B。
- A 改动面太大（meta_keys 值、SessionStore string 接口、tool result JSON、所有 log 字符串都要切 wstring），还会污染上层 LLM/JSON 序列化层；spdlog 全开 `SPDLOG_WCHAR_FILENAMES` 会改变 sink 模板签名，影响 audit logger 与未来扩展
- C 治标不治本：sqlite_open / spdlog 路径仍走 ACP；旧库 meta 已经写坏的数据也修不了
- B 改动局限在 `util/encoding` + 几个"边界文件"（plugin_callbacks / project_context / session_store / project_browser / paths / logging），内部 `std::string` 语义不变；后续 JSON 序列化、tool result、log 文本天然就是 UTF-8

**代价 / 复盘**：
- 必须养成"任何 `fs::path` 从 string 构造都必须走 `fsPathFromUtf8`"的纪律（评审检查点）
- spdlog 路径 fallback 到 `utf8ToAnsi` —— ACP 表示不了的 Unicode 字符（极少见）日志会失败；用户能容忍
- LLM 工具结果中的中文字符串若途经 SDK ANSI API 仍可能乱码（如 `DbgValToString` 一类），留 Group 5 S4 单独处理
- 新增了 `EventBus::DbgEvent::ProjectStoreReady` 通知，避免轮询 `ProjectContext::store()`；选 EventBus 而非 Qt signal 因为项目本来就有总线、不再引入新机制

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
