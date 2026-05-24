// tests/agent_demo/scenario_state_machine.cpp
//
// 场景三：状态机 / 多 callsite -> 同一 helper
//
// 给 AI 看的"谜题"：
//   - Stage3_RunProtocol 内 5 处调用 DoStep(op, &state)，op 各不同
//   - DoStep 内部 switch(op) 根据 op 改 state（init / auth / read / write / close）
//
// 期望 AI 工具调用：
//   1) disasm_at(Stage3_RunProtocol, 60)        -> 看到 5 个 mov ecx, <op> + call DoStep
//   2) list_xrefs_to(DoStep)                    -> 至少 5 个 caller（全在 Stage3_RunProtocol 内）
//   3) disasm_at(DoStep, 40)                    -> 看到 switch dispatch 表 / jump table
//   4) 综合解释：「一个 5 步协议状态机：INIT→AUTH→READ→WRITE→CLOSE」
#include "demo.h"

#include <Windows.h>
#include <cstdio>
#include <cstdint>

#define NOINLINE __declspec(noinline)

namespace agent::s3 {

namespace {

constexpr int OP_INIT  = 1;
constexpr int OP_AUTH  = 2;
constexpr int OP_READ  = 3;
constexpr int OP_WRITE = 4;
constexpr int OP_CLOSE = 9;

// 用 volatile 全局 sink 防 helper 被优化掉
volatile int g_state_sink = 0;

// helper：所有 callsite 都进来这里
// 用 switch 而不是 if-else 链，便于 disasm 看到 jump table / 多 mov+cmp
NOINLINE int DoStep(int op, int* state)
{
    switch (op) {
    case OP_INIT:
        *state = 0x10;
        std::printf("  [DoStep] INIT  -> state=0x%02X\n", *state);
        break;
    case OP_AUTH:
        *state |= 0x20;
        std::printf("  [DoStep] AUTH  -> state=0x%02X\n", *state);
        break;
    case OP_READ:
        *state |= 0x40;
        std::printf("  [DoStep] READ  -> state=0x%02X\n", *state);
        break;
    case OP_WRITE:
        *state |= 0x80;
        std::printf("  [DoStep] WRITE -> state=0x%02X\n", *state);
        break;
    case OP_CLOSE:
        *state = 0xFF;
        std::printf("  [DoStep] CLOSE -> state=0x%02X\n", *state);
        break;
    default:
        std::printf("  [DoStep] UNKNOWN op=%d\n", op);
        return -1;
    }
    g_state_sink += *state;
    return *state;
}

}  // namespace

// 入口：在此处下断
void Stage3_RunProtocol()
{
    std::printf("[S3] Stage3_RunProtocol enter\n");

    int state = 0;

    // 5 个独立 callsite，让 list_xrefs_to(DoStep) 有戏
    DoStep(OP_INIT,  &state);
    DoStep(OP_AUTH,  &state);
    DoStep(OP_READ,  &state);
    DoStep(OP_WRITE, &state);
    DoStep(OP_CLOSE, &state);

    std::printf("[S3] final state=0x%02X\n", state);
    Sleep(200);
}

}  // namespace agent::s3
