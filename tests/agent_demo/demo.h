// tests/agent_demo/demo.h
//
// 跨翻译单元接口声明。所有场景的入口函数都集中暴露在这里，
// main.cpp 按阶段调用，便于在 x64dbg 里逐场景下断、触发 AI 预设。
#pragma once

#include <cstdint>

namespace agent::s1 {
// 场景一：XOR 滚动 key 解码字符串
//
// 入口：Stage1_DecodeAndPrint
//   - 全局密文：g_cipher[]
//   - 全局 key：g_key[]
//   - 解码循环：plain[i] = cipher[i] ^ key[i % key_len] ^ (i & 0xFF)
//
// 期望 AI 工具调用流程（reasoner 模型）：
//   1) disasm_at(Stage1_DecodeAndPrint, 30)   -> 看到循环+XOR
//   2) list_xrefs_from -> 找到 g_cipher / g_key 引用地址
//   3) read_memory(g_cipher, 64)              -> 拿到密文字节
//   4) read_memory(g_key, 16)                 -> 拿到 key
//   5) 解释算法 + 在思考链里口算还原明文
void Stage1_DecodeAndPrint();
}  // namespace agent::s1

namespace agent::s2 {
// 场景二：CRC32（IEEE 802.3 / zlib）表驱动校验
//
// 入口：Stage2_VerifyLicense
//   - 全局表：g_crc_table[256]，预算好的 polynomial=0xEDB88320
//   - SecretCheck(input, len) -> uint32_t  (走标准 CRC32 循环)
//   - Stage2_VerifyLicense 内部：CRC32("HELLO-WORLD-2026") 比对常量 g_expected_crc
//
// 期望 AI 工具调用流程：
//   1) disasm_at(SecretCheck, 20)     -> 看到 ((crc>>8) ^ table[(crc^byte)&0xFF])
//   2) read_memory(g_crc_table, 64)   -> 看头几个 entry：0x00000000, 0x77073096, ...
//   3) AI 应能识别：这是标准 CRC32 表（首个非零 entry 0x77073096 = polynomial 0xEDB88320 倒序的特征）
//   4) read_string(input_str)         -> 看明文
//   5) read_memory(&g_expected_crc, 4) -> 看预期值
void Stage2_VerifyLicense();
}  // namespace agent::s2

namespace agent::s3 {
// 场景三：状态机 / 多 callsite -> 同一 helper
//
// 入口：Stage3_RunProtocol
//   - DoStep(int op, int* state) 是 helper
//   - 5 处不同 callsite 各传不同 op：1=INIT / 2=AUTH / 3=READ / 4=WRITE / 9=CLOSE
//
// 期望 AI 工具调用流程：
//   1) disasm_at(Stage3_RunProtocol, 40)     -> 看到 5 个 call 到 DoStep
//   2) list_xrefs_to(DoStep)                 -> 返回至少 5 个 caller VA
//   3) disasm_at(DoStep, 30)                 -> 看到 switch/jump-table
//   4) 综合解释：这是一个简单 protocol 状态机；列出 5 个 op 的语义
void Stage3_RunProtocol();
}  // namespace agent::s3

namespace agent::s4 {
// 场景四：魔数 / find_pattern 验证
//
// 入口：Stage4_TouchMagics
//   - 全局 kMagicHeader[8]  = DE AD BE EF CA FE BA BE
//   - 全局 kSecondMagic[4]  = 4D 5A 90 00  (伪 MZ)
//   - 代码内联：mov dword ptr [...], 0xDEADBEEF
//                mov dword ptr [...], 0x1337C0DE
//
// 期望 AI 工具调用流程：
//   1) find_pattern("DE AD BE EF CA FE BA BE")    -> 命中 kMagicHeader 的 VA
//   2) find_pattern("4D 5A 90 00")                -> 命中 kSecondMagic
//   3) find_pattern("C7 ?? ?? DE AD BE EF")       -> 命中 mov dword ptr [...], 0xDEADBEEF
//   4) read_memory(命中地址, 16)                  -> 进一步看上下文
//   5) AI 解释：这些魔数像 PE/COFF 风格，可能是文件头校验或某种 sentinel
void Stage4_TouchMagics();
}  // namespace agent::s4
