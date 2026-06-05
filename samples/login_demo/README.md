# Login Demo — x64dbg 调试样本三件套

为 x64dbg-ai-plugin Agent 工具链测试提供的最小化登录程序，3 个独立 exe，
**只有 x64 一个架构**，Qt6 Widgets 编写。校验难度递增。

## 三个 exe

| exe | 校验路径 | Agent 工具链对照 |
|---|---|---|
| `login_demo_plain.exe` | `wcscmp` 直接对硬编码明文 | `scan_strings` / `list_xrefs_to` / `set_breakpoint` / `patch_memory` / `patch_file` |
| `login_demo_xor.exe` | 字节数组 XOR 0x5A 还原后 `memcmp` | `disasm_at` / `read_memory` / `eval_expression` + LLM 推理 |
| `login_demo_antidbg.exe` | plain 之上加 3 层反调试（`IsDebuggerPresent` / PEB.NtGlobalFlag / Timing） | `get_anti_debug_flags` + 逐层 `patch_memory` 绕过 |

正确凭据（全部一致，便于切换对照）：

```
username: admin
password: x64dbg2026
```

## 构建

### 独立构建（推荐）

```pwsh
cmake -S F:\x64dbg_pro\samples\login_demo -B F:\x64dbg_pro\build-samples-x64 `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg-master/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build F:\x64dbg_pro\build-samples-x64 --config Release
```

产物：

```
F:\x64dbg_pro\build-samples-x64\Release\
  login_demo_plain.exe
  login_demo_xor.exe
  login_demo_antidbg.exe
  platforms\qwindows.dll        ← windeployqt 自动拷
```

### 编译选项

- `/Zi /Od` — Release 模式也禁优化 + 留完整 PDB，逆向时反汇编可读
- `/DEBUG /OPT:NOREF /OPT:NOICF` — 链接器不剥符号、不折叠重复函数
- `#pragma optimize("", off)` — 关键校验函数额外加 noinline + 禁优化
- `__declspec(noinline)` — 防止编译器把校验函数内联到 onLogin lambda

## 逆向练习路线（plain 版完整示例）

```text
1. start_debug(login_demo_plain.exe)
   wait_for_event([DebugStarted])

2. scan_strings(module="login_demo_plain")
   → 命中 L"admin" / L"x64dbg2026" / L"Login OK ..."

3. list_xrefs_to(addr_of_password_string)
   → 找到唯一 xref → checkCredentials 内部 wcscmp 调用点

4. set_breakpoint(addr_after_wcscmp_call)
   run_continue → 在 UI 输入 admin/wrong → break
   get_registers → 看 rax（wcscmp 返回值）

5. patch_memory(addr_of_test_eax_eax, "31 C0 90 90")
   或翻转 jne 为 je（74↔75 单字节）
   重新点 Login → 任意密码登录成功

6. patch_file()  把内存补丁落盘到 login_demo_plain_patched.exe
```

## XOR 版破解提示

`.rdata` 里能 dump 出密文数组：

```
22 6C 6E 3E 38 3D 68 6A 68 6C
```

每字节 XOR 0x5A：

```python
>>> bytes(b ^ 0x5A for b in bytes.fromhex("226C6E3E383D686A686C"))
b'x64dbg2026'
```

## Anti-Debug 版三层

| 层 | 实现 | 绕过策略 |
|---|---|---|
| L1 | `IsDebuggerPresent()` | bp kernel32!IsDebuggerPresent → 改 eax=0；或 patch PEB.BeingDebugged=0 |
| L2 | PEB+0xBC `NtGlobalFlag & 0x70 == 0x70` | patch PEB+0xBC 清掉 0x70 位；或在 antiDebugL2 入口直接 ret 0 |
| L3 | `QueryPerformanceCounter` Timing | 单步跨过会必中；解法：跳过整个 L3 调用（patch call → nop nop nop nop nop） |

阈值 200ms 较宽松，正常运行不会误触发。

## 备注

- 三个 exe **故意不共享代码**，PDB 干净，逆向时不会出现"为什么这个函数也在 plain.exe 里"的困惑
- 未做 anti-attach，`attach_debug(pid)` 可用
- 校验函数返回 int 而不是 bool，方便区分"用户名错"和"密码错"两条路径（典型 crackme 漏洞）
