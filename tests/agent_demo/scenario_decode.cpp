// tests/agent_demo/scenario_decode.cpp
//
// 场景一：XOR + 滚动 key 字符串解码
//
// 给 AI 看的"谜题"：
//   - 函数名 Stage1_DecodeAndPrint / DecodeBuf 都是中性命名，看不出意图
//   - 解码算法藏在循环里：cipher[i] ^ key[i % key_len] ^ (i & 0xFF)
//   - 调用工具链验证 AI 能否："读密文 + 读 key + 看汇编 -> 还原算法 -> 给出明文"
//
// 明文（仅 demo 作者知道，AI 应通过工具调用 + 思考链推出来）：
//   "Hello, x64dbg agent! Tools work."  (共 33 字节，含尾 0)
#include "demo.h"

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#define NOINLINE __declspec(noinline)

namespace agent::s1 {

namespace {

// 密文：用下面 Python 等价逻辑生成
//   key = bytes.fromhex("3F A1 5C 7E B2")
//   plain = b"Hello, x64dbg agent! Tools work.\x00"
//   cipher = bytes(p ^ key[i % len(key)] ^ (i & 0xFF) for i, p in enumerate(plain))
//
// 用 volatile 防止编译器在初始化阶段就识别出常量做优化合并
volatile uint8_t g_cipher[33] = {
    0x77, 0xC5, 0x32, 0x11, 0xD9,
    0x16, 0x87, 0x23, 0x40, 0x8F,
    0x51, 0xC8, 0x37, 0x53, 0xDD,
    0x57, 0xD4, 0x23, 0x18, 0x80,
    0x0B, 0xE0, 0x25, 0x06, 0xC6,
    0x55, 0x9B, 0x30, 0x0D, 0xDD,
    0x4A, 0x90, 0x7C,
};

// 5 字节滚动 key
volatile uint8_t g_key[5] = { 0x3F, 0xA1, 0x5C, 0x7E, 0xB2 };

// 解码 helper：单独成函数便于 AI 用 disasm_at 锁定核心循环
NOINLINE void DecodeBuf(const volatile uint8_t* cipher,
                        const volatile uint8_t* key,
                        size_t key_len,
                        size_t n,
                        uint8_t* out)
{
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = cipher[i];
        uint8_t k = key[i % key_len];
        uint8_t m = static_cast<uint8_t>(i & 0xFF);
        out[i] = static_cast<uint8_t>(c ^ k ^ m);
    }
}

}  // namespace

// 入口：在此处下断 -> 右键 AI ▶ 分析当前函数 / 自由提问"这个函数做什么"
void Stage1_DecodeAndPrint()
{
    std::printf("[S1] Stage1_DecodeAndPrint enter\n");

    uint8_t plain[64] = {0};
    DecodeBuf(g_cipher, g_key, sizeof(g_key), sizeof(g_cipher), plain);

    std::printf("[S1] decoded -> %s\n", reinterpret_cast<const char*>(plain));
    Sleep(200);
}

}  // namespace agent::s1
