// tests/agent_demo/scenario_magic.cpp
//
// 场景四：魔数 + 代码模式（专测 find_pattern 工具）
//
// 给 AI 看的"谜题"：
//   - 全局数组 kMagicHeader / kSecondMagic / kThirdMagic 各自有显眼的字节序列
//   - Stage4_TouchMagics 函数内有 "mov dword ptr [...], 0xDEADBEEF" 这种立即数写入
//     在反汇编里会出现 C7 4? ?? DE AD BE EF 这样的字节模式
//
// 期望 AI 工具调用：
//   1) find_pattern("DE AD BE EF CA FE BA BE")        -> 命中 kMagicHeader VA
//   2) find_pattern("4D 5A 90 00")                    -> 命中 kSecondMagic VA
//   3) find_pattern("13 37 C0 DE")                    -> 命中 kThirdMagic
//   4) find_pattern("C7 ?? ?? DE AD BE EF") 或类似   -> 命中代码段里的立即数 mov
//   5) read_memory(命中地址, 16-32)                   -> 读上下文
//   6) AI 总结：「这些魔数像 PE 头/leet 标记/sentinel；代码段把 0xDEADBEEF 写入栈/全局」
#include "demo.h"

#include <Windows.h>
#include <cstdio>
#include <cstdint>

#define NOINLINE __declspec(noinline)

namespace agent::s4 {

// 故意用 alignas 让搜起来好认
alignas(16) volatile uint8_t kMagicHeader[8] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE
};

alignas(16) volatile uint8_t kSecondMagic[4] = {
    0x4D, 0x5A, 0x90, 0x00   // 伪 MZ
};

alignas(16) volatile uint8_t kThirdMagic[4] = {
    0x13, 0x37, 0xC0, 0xDE   // leetcode
};

// volatile sink：防止内联 + 防整段被优化
volatile uint32_t g_sink32 = 0;

// 这个函数会产生形如：
//   C7 45 F0 EF BE AD DE      mov [rbp-10], 0xDEADBEEF
//   C7 45 F4 DE C0 37 13      mov [rbp-0C], 0x1337C0DE
// 之类的字节序列，便于 find_pattern 验证通配
NOINLINE void EmitImmediateWrites()
{
    volatile uint32_t a = 0xDEADBEEFu;
    volatile uint32_t b = 0x1337C0DEu;
    volatile uint32_t c = 0xCAFEBABEu;
    g_sink32 += a + b + c;
}

// 入口：在此处下断 -> 右键 AI ▶ 自由提问
//   "在内存里搜一下 DEADBEEFCAFEBABE 找到了什么"
//   或 "用 find_pattern 找几个常见魔数，告诉我它们在哪、属于哪些数据"
void Stage4_TouchMagics()
{
    std::printf("[S4] Stage4_TouchMagics enter\n");

    EmitImmediateWrites();

    // 触摸一下三个魔数，避免被 linker DCE 干掉
    g_sink32 += kMagicHeader[0] + kSecondMagic[0] + kThirdMagic[0];

    std::printf("[S4] kMagicHeader @ %p  kSecondMagic @ %p  kThirdMagic @ %p\n",
                (void*)kMagicHeader, (void*)kSecondMagic, (void*)kThirdMagic);
    std::printf("[S4] sink32 = 0x%08X\n", g_sink32);
    Sleep(200);
}

}  // namespace agent::s4
