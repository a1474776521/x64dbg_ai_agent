// tests/reasoning_demo/main.cpp
//
// Reasoning UI 测试程序（thinking 模型专用）
// ============================================================
// 设计目标：
//   为 x64dbg AI 插件的 M4.6 Reasoning UI（ReasoningBlock 折叠面板）
//   提供"必须长篇推理"的小函数。函数名全部中性、不给注释提示，
//   让 deepseek-reasoner 等 thinking 模型必须在 reasoning_content
//   里大量推导，从而充分验证：
//     - reasoning_content 流式增量是否实时显示
//     - 标题"思考过程 (N)" 字符计数是否准确
//     - 默认折叠 + 点击展开是否正常
//     - 多轮工具调用情况下 reasoning_content 必须回传（无 HTTP 400）
//
// 三道"烧脑题"：
//   T1: Mystery1  -> 位运算：(x | (x >> 16))，其实是 nextPow2/ceiling 的子步骤
//                    一眼看不出是什么经典算法，reasoner 必须逐位 trace
//   T2: Mystery2  -> 递归 + 简单备忘录：看着像 Fibonacci 但加了偏移
//                    reasoner 需要展开几步才能确认序列
//   T3: Mystery3  -> 循环约简：辗转相除（GCD），但变量名是 a/b/t/r 没暗示
//
// 操作流程（与 agent_demo 类似）：
//   1) 启动 reasoning_demo.exe
//   2) x64dbg attach
//   3) provider=DeepSeek, model=deepseek-reasoner
//   4) 在 Mystery1/2/3 入口下断
//   5) 命中后右键 AI ▶ 「分析当前函数」 或 自由提问 "这个函数算什么？"
//   6) 观察 AssistantPanel 助手气泡上方 "▶ 思考过程 (xxx)" 折叠面板
//      - 字符数实时增长
//      - 流式结束后保留，可点击展开
//      - 完整推理过程清晰可读
// ============================================================
#include <Windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#define NOINLINE __declspec(noinline)

namespace rsn {

// volatile sink，防优化整段消失
volatile uint64_t g_sink = 0;

// T1: 看起来在做位扩散 / popcount-ish 的小函数
// 实际上是 next-power-of-2 ceiling 算法的核心：
//   把最高位向下"漫延"填满后再 +1
// 反汇编里只看 OR/SHR/MOV 序列，没有任何线索说明意图
NOINLINE uint32_t Mystery1(uint32_t v)
{
    if (v == 0) return 1u;
    v -= 1u;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v += 1u;
    return v;
}

// T2: 递归 + 备忘表
// 表达式 f(n) = f(n-1) + f(n-2) + (n & 1)
// 不是标准 Fibonacci，reasoner 需展开 4-5 项才能确认
NOINLINE uint64_t Mystery2Helper(uint32_t n, uint64_t memo[64])
{
    if (n < 2) return n;
    if (memo[n] != 0xFFFFFFFFFFFFFFFFull) return memo[n];
    uint64_t r = Mystery2Helper(n - 1, memo) + Mystery2Helper(n - 2, memo) + (n & 1u);
    memo[n] = r;
    return r;
}

NOINLINE uint64_t Mystery2(uint32_t n)
{
    uint64_t memo[64];
    for (auto& x : memo) x = 0xFFFFFFFFFFFFFFFFull;
    if (n >= 64) n = 63;
    return Mystery2Helper(n, memo);
}

// T3: 循环约简 -- 经典 GCD（辗转相除）但变量名中性
// reasoner 必须看到 a%b 然后 swap 才能识别
NOINLINE uint64_t Mystery3(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

}  // namespace rsn

namespace {

void Pause(const char* hint, bool wait)
{
    if (!wait) return;
    std::printf("\n>>> %s  (PRESS ENTER) <<<\n", hint);
    (void)std::getchar();
}

}  // namespace

int main(int argc, char** argv)
{
    // 控制台输出 UTF-8（源文件是 UTF-8 无 BOM + /utf-8，需要让 cmd 用 CP 65001 才不乱码）
    SetConsoleOutputCP(CP_UTF8);

    bool wait = true;
    bool loop = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-wait") wait = false;
        else if (a == "--loop") loop = true;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: reasoning_demo.exe [--no-wait] [--loop]\n"
                "  --no-wait   skip Enter prompts\n"
                "  --loop      run forever\n");
            return 0;
        }
    }

    std::printf("reasoning_demo (PID=%lu)\n", GetCurrentProcessId());
    std::printf("Attach x64dbg now; set breakpoints on rsn::Mystery1/2/3.\n");
    std::printf("Recommend: provider=DeepSeek, model=deepseek-reasoner\n");
    Pause("ready?", wait);

    int round = 0;
    do {
        if (loop) std::printf("\n========== Round %d ==========\n", ++round);

        Pause("T1: rsn::Mystery1(13) — 请下断后回车", wait);
        uint32_t r1 = rsn::Mystery1(13);
        std::printf("[T1] Mystery1(13) = %u\n", r1);   // 期望 16
        rsn::g_sink += r1;

        Pause("T2: rsn::Mystery2(10) — 请下断后回车", wait);
        uint64_t r2 = rsn::Mystery2(10);
        std::printf("[T2] Mystery2(10) = %llu\n", (unsigned long long)r2);
        rsn::g_sink += r2;

        Pause("T3: rsn::Mystery3(840, 360) — 请下断后回车", wait);
        uint64_t r3 = rsn::Mystery3(840, 360);
        std::printf("[T3] Mystery3(840, 360) = %llu\n", (unsigned long long)r3);  // 期望 120
        rsn::g_sink += r3;

        if (loop) Sleep(1000);
    } while (loop);

    std::printf("\nsink = %llu\n", (unsigned long long)rsn::g_sink);
    Pause("ENTER to exit", wait);
    return 0;
}
