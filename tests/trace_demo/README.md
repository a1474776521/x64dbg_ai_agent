# trace_demo —— 调用链追溯测试程序

用于验证 x64dbg AI 插件 **"AI: 追溯函数调用链"** 功能。

## 测试覆盖

| 测试段 | 入口函数 | 验证场景 | 预期 CallGraph |
|--------|----------|---------|----------------|
| Test1 | `demo::business::ProcessOrder` | 跨 TU、多分支、深度 3 | Validate/CalcTotal/ApplyDiscount/SaveToDb 及其子调用 |
| Test2 | `demo::poly::DoPolyCall`        | C++ 虚函数（间接 call） | Dog::Speak / Cat::Speak |
| Test3 | `demo::cb::RunPipeline`         | 函数指针 + std::function | StageA/B/C |
| Test4 | `demo::math::Factorial`         | 递归                    | 5 次自调用 |
| Test5 | `demo::math::LayerA`            | 跨 TU 深度链            | LayerB/C/LeafD |

## 编译

```pwsh
cd tests\trace_demo
cmake -B build -S . -A x64
cmake --build build --config Release
# 产物：build\Release\trace_demo.exe
```

或单文件 cl 编译：
```pwsh
cl /std:c++20 /EHsc /Od /Zi /Fe:trace_demo.exe main.cpp business.cpp math_ops.cpp
```

也可以编译 32 位（用 `-A Win32`）。

## 测试流程

1. 启动 `trace_demo.exe`，它会打印 PID 并等待回车
2. x64dbg → File → Attach 选中 PID
3. 在感兴趣的入口函数下断（如 `demo::business::ProcessOrder`）
4. 切回 demo 控制台 **按回车**，让程序跑到断点
5. x64dbg 命中后：在反汇编窗口右键 → **AI: 追溯函数调用链**
6. 在弹出对话框选 "Targeted"（针对当前函数），点 Start
7. 程序继续运行（F9 或自动），完成函数调用后追溯自动停止
8. 查看 CallGraph 输出，对照"预期"列验证

## 命令行参数

```
trace_demo.exe [--rounds=N] [--no-wait]
  --rounds=N   循环跑 N 轮（默认 1）
  --no-wait    不等待回车（适合自动化）
```

## 调试建议

- 编译时已强制 `/Od`，所有 call 指令保留，无 inline。
- 如果某个间接 call（虚函数 / 函数指针）没出现在 CallGraph，说明 plugin 的 `info.call` + 助记符兜底仍漏，请把 `%APPDATA%\x64dbg-ai-plugin\logs\plugin.log` 的 `step #` 心跳贴出来。
