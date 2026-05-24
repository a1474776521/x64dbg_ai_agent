# agent_demo —— M4 Agent 工具调用 / 预设 / 思考链测试程序

为 x64dbg AI 插件的 **M4 多轮工具调用 + 预设管理 + Reasoning UI** 提供"谜题型"测试目标。

## 验证目标

| 功能 | 在哪验证 |
|---|---|
| **反汇编右键 AI ▶ 子菜单**（M4.6f） | 任意 StageN 入口下断，命中后右键反汇编窗口看 `AI ▶` 子菜单是否列出预设；点选触发 |
| **PresetEditorDialog 增删改**（M4.6e） | 顶部「预设管理」按钮，创建/复制/删除/恢复出厂；保存后回 demo 触发新预设 |
| **enabledTools 透传** | 在预设里只勾 `read_memory + disasm_at`，跑 S2，确认 LLM 拿不到 list_xrefs_to 工具调用 |
| **Agent 多轮工具调用 + ToolCallCard** | S1/S2/S3/S4 都会触发多次工具调用，看卡片折叠/展开/状态色 |
| **思考过程折叠面板（ReasoningBlock）** | provider=DeepSeek + model=deepseek-reasoner，跑任一 stage，看上方灰色折叠面板字符数实时增长 |
| **reasoning_content 必须回传**（无 HTTP 400） | reasoner + 多轮工具调用必须不报 400，否则就是回传链断了 |

## 场景

| ID | 入口函数 | 场景说明 | 预期工具调用 |
|---|---|---|---|
| **S1** | `agent::s1::Stage1_DecodeAndPrint` | XOR + 滚动 key 解码字符串。明文 `"Hello, x64dbg agent! Tools work."` | `disasm_at` → `read_memory(g_cipher)` → `read_memory(g_key)` → 思考链口算还原 |
| **S2** | `agent::s2::Stage2_VerifyLicense` | 标准 CRC32（polynomial=0xEDB88320）校验 `"HELLO-WORLD-2026"` | `disasm_at(SecretCheck)` → `read_memory(g_crc_table, 64)` → AI 识别为标准 CRC32 → `read_string` 看明文 |
| **S3** | `agent::s3::Stage3_RunProtocol` | 5 个 callsite 调同一 helper `DoStep`，op=INIT/AUTH/READ/WRITE/CLOSE | `disasm_at(Stage3)` → `list_xrefs_to(DoStep)` → `disasm_at(DoStep)` → 解释状态机 |
| **S4** | `agent::s4::Stage4_TouchMagics` | 3 个魔数 + 一段立即数 mov | `find_pattern("DE AD BE EF ...")` 多次 → `read_memory` 看上下文 |

## 编译

```pwsh
cd F:\x64dbg_pro\tests\agent_demo
cmake -B build -S . -A x64
cmake --build build --config Release
# 产物：build\Release\agent_demo.exe
```

32 位用 `-A Win32`。

## 推荐测试流程

### 0) 准备
1. 启动 `agent_demo.exe`（无参，会等回车）
2. 记住打印的 PID
3. 打开 x64dbg → File → Attach → 选 PID
4. 加载本插件（应已自动加载）
5. 打开 AssistantPanel：确认 provider/model（**推荐 DeepSeek + deepseek-reasoner**，能验证 ReasoningBlock）

### 1) 验证反汇编右键 AI ▶ 子菜单
1. 反汇编窗口 Ctrl+G → `agent::s1::Stage1_DecodeAndPrint`
2. F2 下断
3. 切回 console，按回车放过 `S1` 前的 Pause；让程序命中断点
4. 反汇编窗口右键 → 应看到 `AI ▶` 子菜单，列出所有 `showInContextMenu=true` 的预设
5. 点 **「分析当前函数」** → AssistantPanel 自动跑 agent；观察：
   - ToolCallCard 依次出现（pending → running → done）
   - 若用 reasoner：上方 `▶ 思考过程 (N)` 字符数实时增长
   - 最终给出"这是 XOR 解码"的解释

### 2) 验证 PresetEditorDialog
1. AssistantPanel 顶部「预设管理」按钮 → 对话框打开
2. **新建**：点新建，命名 `测试预设_S2`，systemPrompt 写"专门解释加密算法，必须用工具验证算法细节"，enabledTools 只勾 `read_memory + disasm_at + read_string`，保存
3. **showInContextMenu** 勾上 → 关闭对话框 → 反汇编右键 AI ▶ 应**立即**看到新预设条目（验证 rebuildDisasmAiSubmenu）
4. 在 `Stage2_VerifyLicense` 命中，右键 → 选刚建的预设
5. 观察：LLM 不应该出现 `list_xrefs_to` 或 `find_pattern` 调用（被工具屏蔽了）
6. 回到对话框 → **复制副本** → 改名 → 保存
7. **删除**用户预设 → 关闭/重开 AssistantPanel → 确认消失
8. **恢复出厂** → 应只覆盖 5 个 readonly，用户预设保留

### 3) 验证 Reasoning UI（必须 deepseek-reasoner）
1. 顶部 provider=DeepSeek，model=deepseek-reasoner
2. 任意 stage 命中 → 右键跑预设
3. 观察：
   - 助手气泡上方挂 `▶ 思考过程 (xxx)` 折叠按钮
   - 流式期间字符数实时增长
   - 点击展开看完整 CoT
   - 多轮工具调用过程中**不应出现** HTTP 400 invalid_request_error（验证 reasoning_content 回传链路）

### 4) 验证 find_pattern（S4 专属）
1. `Stage4_TouchMagics` 下断 → 命中
2. AssistantPanel 输入框打字：
   > 请用 find_pattern 工具搜索几个常见魔数（比如 `DE AD BE EF CA FE BA BE`、`4D 5A 90 00`、`13 37 C0 DE`），找到后用 read_memory 看上下文，告诉我它们分别是什么。
3. 观察 ToolCallCard 应出现至少 3 次 find_pattern + 多次 read_memory

## 命令行参数

```
agent_demo.exe [--no-wait] [--loop] [--only=N]
  --no-wait   不在每个 stage 前等回车（适合自动化）
  --loop      所有 stage 跑完循环再来
  --only=N    只跑 stage N（1..4）
```

## 调试建议

- 编译 `/Od`，所有 helper 函数与立即数 mov 都保留
- 全局数组用 `volatile`，防止编译期常量折叠让 AI 看不见
- 函数名故意中性（`SecretCheck` / `DoStep` / `EmitImmediateWrites`），看 PDB 也猜不出语义
- 看插件日志：`%APPDATA%\x64dbg-ai-plugin\logs\plugin.log`，每次 agent 调用都有 `tool=... args=... result=... ms=...` 一行
