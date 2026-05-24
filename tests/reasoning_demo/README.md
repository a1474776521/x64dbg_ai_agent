# reasoning_demo —— thinking 模型思考链测试程序

专门验证 **ReasoningBlock 折叠面板**（M4.6 Reasoning UI）。

## 目标

让 `deepseek-reasoner` 等 thinking 模型对几段**无注释、命名中性**的小函数推理一番，从而充分驱动：

- `reasoning_content` 流式增量是否实时显示到 `▶ 思考过程 (N)` 标题字符数
- 默认折叠是否正常 / 点击展开能否看到完整 CoT
- 流式结束后保留状态是否正确
- 多轮工具调用下 **reasoning_content 必须回传**（否则 HTTP 400 invalid_request_error）

## 三道烧脑题

| 入口 | 输入 | 真实算法 | 期望思考链长度 |
|---|---|---|---|
| `rsn::Mystery1(13)` | 13 → 期望返回 16 | next-power-of-2 ceiling（位漫延） | 中等（应展开二进制 trace） |
| `rsn::Mystery2(10)` | 10 → 返回某个序列值 | 类 Fibonacci + (n & 1) 偏移 | 长（应列前几项推导） |
| `rsn::Mystery3(840, 360)` | 期望 120 | 辗转相除 GCD | 中等（迭代演算） |

函数名全部 `MysteryN`，变量名 `a/b/t/r/v` 无暗示，PDB 里只看到符号位置不见语义。

## 编译

```pwsh
cd F:\x64dbg_pro\tests\reasoning_demo
cmake -B build -S . -A x64
cmake --build build --config Release
# 产物：build\Release\reasoning_demo.exe
```

## 操作流程

1. 启动 `reasoning_demo.exe`（无参，等回车）
2. 记住 PID → x64dbg Attach
3. AssistantPanel：**provider=DeepSeek，model=deepseek-reasoner**（关键！普通 chat 模型没有 reasoning_content）
4. 反汇编 Ctrl+G → `rsn::Mystery1` → F2 下断
5. 回 console 按回车，命中后右键反汇编 → AI ▶ 「分析当前函数」（或自由提问"这个函数计算什么？"）
6. **观察 AssistantPanel**：
   - 助手气泡上方应立即出现灰色折叠按钮 `▶ 思考过程 (xxx)`
   - 字符数随 SSE 流式实时增长
   - 流式结束后助手气泡下方出现最终结论
   - 点击折叠按钮 → 展开看完整 reasoning 内容（应能看到位 trace / 序列展开 / 辗转相除推导等）

## 期望结论

- T1：Mystery1 是 `next_power_of_two(13) = 16`
- T2：Mystery2 应给出确切数值 + 序列公式 `f(n) = f(n-1) + f(n-2) + (n & 1)`
- T3：Mystery3 = GCD(840, 360) = 120

如果 reasoner 给的结论错了（特别是 T2 数值），通常意味着它没真正展开足够多项；不算 ReasoningBlock 的 bug，记录到 known-issues 即可。

## 反例：验证 reasoning_content 回传

打开"分析当前函数"预设（默认启用 disasm_at / read_memory 等工具）跑 T3，会产生 ≥2 轮工具调用。如果第 2 轮直接 HTTP 400 报 `"The reasoning_content in the thinking mode must be passed back to the API"`，说明 AgentLoop 回传链路坏了（K-12 已修复过，这里是回归点）。

## 提示

- 故意 `/Od`，所有位运算/循环/递归都保留显式指令
- `g_sink` 累加防整段被 DCE
- 想看不同输入下的推理：手动改 main.cpp 调用参数后重编
