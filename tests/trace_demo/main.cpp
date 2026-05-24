// tests/trace_demo/main.cpp
//
// 调用链追溯功能测试程序
// ============================================================
// 设计目标：
//   1) 提供清晰的多层函数调用，便于在 x64dbg 中对任意中间层
//      函数下断 + 启动 Trace 追溯，观察 plugin 的 CallGraph 输出。
//   2) 覆盖直接调用、嵌套调用、递归、函数指针、虚函数、跨 TU 调用
//      等典型场景，验证 call/ret 识别覆盖率。
//   3) 在每次"业务动作"前后用 Sleep 拉开间隔，便于人工断点。
//
// 推荐测试用例（在 x64dbg 中操作）：
//   A. 在 demo::business::ProcessOrder 入口下断 -> 命中后右键 ->
//      "AI: 追溯函数调用链" -> 应看到 ValidateOrder / CalcTotal /
//      ApplyDiscount / SaveToDb 等子调用。
//   B. 在 demo::math::Factorial 入口下断 -> 应看到递归子调用 4 层。
//   C. 在 demo::poly::Animal::Speak 多态点下断 -> 验证虚函数调用
//      也能识别 callee。
//   D. 在 demo::cb::RunPipeline 入口下断 -> 验证函数指针 call rax
//      也能落入 CallGraph（依赖助记符兜底识别）。
//
// 编译（VS2022 x64 Release，关闭优化便于看到所有 call）：
//   cl /std:c++20 /EHsc /Od /Zi /Fe:trace_demo.exe ^
//      main.cpp business.cpp math_ops.cpp
//
// 也可以用 CMakeLists.txt（同目录已提供）。
// ============================================================

#include <Windows.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "demo.h"

namespace demo::poly {

// ---- 多态调用链：基类 + 两个派生类 ----
struct Animal {
    virtual ~Animal() = default;
    virtual void Speak() = 0;
};

struct Dog : Animal {
    void Speak() override
    {
        std::printf("[Dog]   Woof! Woof!\n");
    }
};

struct Cat : Animal {
    void Speak() override
    {
        std::printf("[Cat]   Meow~\n");
    }
};

// 入口：被测函数。在此处下断 -> 追溯应看到 Dog::Speak / Cat::Speak
void DoPolyCall()
{
    std::vector<std::unique_ptr<Animal>> zoo;
    zoo.emplace_back(std::make_unique<Dog>());
    zoo.emplace_back(std::make_unique<Cat>());
    zoo.emplace_back(std::make_unique<Dog>());

    for (auto& a : zoo) {
        a->Speak();   // 间接 call (call qword ptr [rax+...])
    }
}

}  // namespace demo::poly

namespace demo::cb {

// ---- 函数指针 + std::function 调用链 ----
using StageFn = void(*)(int);

static void StageA(int x) { std::printf("[StageA] x=%d\n", x); }
static void StageB(int x) { std::printf("[StageB] x=%d\n", x * 2); }
static void StageC(int x) { std::printf("[StageC] x=%d\n", x + 100); }

// 入口：被测函数。在此处下断 -> 应看到 StageA / StageB / StageC 与
// std::function 内部跳板调用
void RunPipeline()
{
    StageFn stages[] = { &StageA, &StageB, &StageC };
    for (auto fn : stages) {
        fn(7);   // call rax / call qword ptr [...]
    }

    std::function<void(int)> wrap = &StageB;
    wrap(42);
}

}  // namespace demo::cb

namespace demo::math {

// ---- 递归调用链 ----
// 入口：被测函数。在此处下断 -> 追溯应看到 4 次 Factorial 自调用
unsigned long long Factorial(unsigned n)
{
    if (n <= 1) return 1ULL;
    return n * Factorial(n - 1);
}

// LayerA 实现移到 math_ops.cpp，便于跨 TU 验证

}  // namespace demo::math

namespace demo {

// 主测试驱动：每个测试段前后 Sleep，方便手动控制
void RunOneRound(int round)
{
    std::printf("\n========== Round %d ==========\n", round);

    // [Test 1] 业务调用链（跨 TU）
    std::printf("\n-- [Test1] business::ProcessOrder --\n");
    Sleep(500);
    business::ProcessOrder(1000 + round, /*qty=*/3, /*premium=*/round % 2 == 0);

    // [Test 2] 多态调用
    std::printf("\n-- [Test2] poly::DoPolyCall --\n");
    Sleep(500);
    poly::DoPolyCall();

    // [Test 3] 函数指针管线
    std::printf("\n-- [Test3] cb::RunPipeline --\n");
    Sleep(500);
    cb::RunPipeline();

    // [Test 4] 递归
    std::printf("\n-- [Test4] math::Factorial(6) --\n");
    Sleep(500);
    auto f = math::Factorial(6);
    std::printf("Factorial(6) = %llu\n", f);

    // [Test 5] 静态嵌套链
    std::printf("\n-- [Test5] math::LayerA(10) --\n");
    Sleep(500);
    auto la = math::LayerA(10);
    std::printf("LayerA(10) = %d\n", la);

    std::printf("\n========== Round %d done ==========\n", round);
}

}  // namespace demo

int main(int argc, char** argv)
{
    int rounds = 1;
    bool wait = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--rounds=", 0) == 0) {
            rounds = std::atoi(a.c_str() + 9);
        } else if (a == "--no-wait") {
            wait = false;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: trace_demo.exe [--rounds=N] [--no-wait]\n"
                "  --rounds=N   Run test rounds N times (default 1)\n"
                "  --no-wait    Do not pause for keypress before/after\n");
            return 0;
        }
    }

    std::printf("trace_demo (PID=%lu)\n", GetCurrentProcessId());
    std::printf("Attach x64dbg now. PRESS ENTER to start tests...\n");
    if (wait) (void)std::getchar();

    for (int i = 1; i <= rounds; ++i) {
        demo::RunOneRound(i);
        if (i != rounds) Sleep(800);
    }

    std::printf("\nAll done. PRESS ENTER to exit...\n");
    if (wait) (void)std::getchar();
    return 0;
}
