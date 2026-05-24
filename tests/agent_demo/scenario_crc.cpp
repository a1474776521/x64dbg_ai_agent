// tests/agent_demo/scenario_crc.cpp
//
// 场景二：表驱动 CRC32（IEEE 802.3 / zlib 标准）
//
// 给 AI 看的"谜题"：
//   - SecretCheck 看着像个普通校验函数
//   - 内部循环：((crc >> 8) ^ table[(crc ^ byte) & 0xFF])，标准 CRC32 right-shifting 形式
//   - 表前几项是 0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, ... （polynomial=0xEDB88320 的特征）
//
// 期望 AI 工具调用：
//   1) disasm_at(SecretCheck)           -> 看 XOR / SHR / AND / table 索引
//   2) read_memory(g_crc_table, 16-64)  -> 看表头几项
//   3) AI 在思考链识别为标准 CRC32
//   4) read_string + read_memory(g_expected_crc, 4) -> 给出输入字符串与预期值
//   5) AI 可以在思考链里"心算"CRC，或者直接说"这是 HELLO-WORLD-2026 的 CRC32"
#include "demo.h"

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#define NOINLINE __declspec(noinline)

namespace agent::s2 {

namespace {

// IEEE 802.3 / zlib CRC32 polynomial = 0xEDB88320
// 用 volatile 防止表被合并/优化
volatile uint32_t g_crc_table[256];
volatile bool g_table_inited = false;

NOINLINE void InitCrcTable()
{
    if (g_table_inited) return;
    const uint32_t poly = 0xEDB88320u;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_table_inited = true;
}

// 标准 CRC32 (right-shifting)：初值 0xFFFFFFFF，结果 XOR 0xFFFFFFFF
NOINLINE uint32_t SecretCheck(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        crc = (crc >> 8) ^ g_crc_table[(crc ^ b) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

// 待校验的"许可证字符串"
volatile char g_license[] = "HELLO-WORLD-2026";

// 预期值（CRC32("HELLO-WORLD-2026") = 0x29F1A4A1，下面运行时再算一次比对）
// 之所以不写死：避免编译期常量折叠后 AI 看不见运行流程；让它确实跑一次 SecretCheck
volatile uint32_t g_expected_crc = 0;

}  // namespace

// 入口：在此处下断 -> 右键 AI ▶ 分析当前函数
void Stage2_VerifyLicense()
{
    std::printf("[S2] Stage2_VerifyLicense enter\n");

    InitCrcTable();

    // 先算一次塞进 g_expected_crc（让"标准答案"出现在内存里，AI 可以 read_memory 验证）
    if (g_expected_crc == 0) {
        g_expected_crc = SecretCheck(
            reinterpret_cast<const uint8_t*>(const_cast<const char*>(g_license)),
            std::strlen(const_cast<const char*>(g_license)));
    }

    uint32_t crc = SecretCheck(
        reinterpret_cast<const uint8_t*>(const_cast<const char*>(g_license)),
        std::strlen(const_cast<const char*>(g_license)));

    std::printf("[S2] license=%s  crc=0x%08X  expected=0x%08X  %s\n",
                const_cast<const char*>(g_license),
                crc, g_expected_crc,
                (crc == g_expected_crc) ? "OK" : "MISMATCH");
    Sleep(200);
}

}  // namespace agent::s2
